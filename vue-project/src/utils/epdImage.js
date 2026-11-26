/**
 * E-Paper 1bpp bitplane quantization (Black/White & Black/White/Red) + PackBits RLE encoder.
 * Framebuffer convention matches SSD1680 / SSD1683 HAL (`hal_epd.c`):
 *   - BW plane:  1 = White, 0 = Black (for Red pixels, BW bit = 1)
 *   - Red plane: 1 = Red,   0 = None
 * MSB-first per byte, row-major with row stride = Math.ceil(width / 8).
 */

export const EPD_PAPER_HEX = {
  white: [246, 244, 236], // #f6f4ec warm e-ink paper
  black: [24, 24, 27],    // #18181b dark carbon ink
  red: [185, 28, 28],     // #b91c1c vermilion tri-color ink
}

function clampByte(v) {
  return v < 0 ? 0 : v > 255 ? 255 : v
}

function clampErr(v) {
  return v < -85 ? -85 : v > 85 ? 85 : v
}

/**
 * PackBits RLE compressor for 1bpp EPD planes.
 * Header byte `h` (interpreted as signed int8 in C `decode_packbits`):
 *   0..127   -> copy (h + 1) literal bytes
 *   -1..-127 -> repeat next 1 byte (1 - h) times (2..128)
 */
export function encodePackBits(bytes) {
  const src = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes)
  const out = []
  let i = 0
  while (i < src.length) {
    let run = 1
    while (i + run < src.length && run < 128 && src[i + run] === src[i]) {
      run++
    }
    if (run >= 2) {
      out.push((257 - run) & 0xff, src[i])
      i += run
    } else {
      const litStart = i
      let litLen = 0
      while (i < src.length && litLen < 128) {
        if (i + 1 < src.length && src[i] === src[i + 1]) {
          break
        }
        i++
        litLen++
      }
      if (litLen === 0) {
        litLen = 1
        i++
      }
      out.push(litLen - 1)
      for (let k = 0; k < litLen; k++) {
        out.push(src[litStart + k])
      }
    }
  }
  return new Uint8Array(out)
}

export function bytesToBase64(bytes) {
  const arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes)
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(arr).toString('base64')
  }
  let bin = ''
  const chunk = 0x8000
  for (let i = 0; i < arr.length; i += chunk) {
    bin += String.fromCharCode(...arr.subarray(i, i + chunk))
  }
  return btoa(bin)
}

function pickBwPixel(luma, threshold = 128) {
  return luma < threshold ? 1 : 0 // 1=black, 0=white
}

/**
 * Perceptual (Luma, Red-Chroma) nearest-color selector for B/W/R e-paper.
 * Avoids Euclidean RGB turning neutral mid-grays reddish while keeping reds vivid.
 */
function pickBwrPixel(r, g, b, redSensitivity = 1.0) {
  const sens = Math.max(0.4, Math.min(2.0, Number(redSensitivity) || 1.0))
  const luma = 0.299 * r + 0.587 * g + 0.114 * b
  const redness = r - 0.5 * (g + b)

  const dW = (luma - 255) ** 2 + redness ** 2
  const dK = luma ** 2 + redness ** 2
  // Adjust effective red center / sensitivity inversely so higher sensitivity catches warmer reds
  const redCenterC = Math.max(90, 195 / sens)
  const dR = ((luma - 82) ** 2 + (redness - redCenterC) ** 2) * (redness > 12 ? 1.0 : 4.0)

  if (dR <= dK && dR <= dW && redness > 18) return 2 // RED
  return dK <= dW ? 1 : 0 // BLACK : WHITE
}

/**
 * Quantize RGBA pixel buffer into EPD bitplanes + paper-style preview RGBA.
 * @param {Uint8ClampedArray|Uint8Array|number[]} rgba
 * @param {number} width
 * @param {number} height
 * @param {object} [options]
 */
export function quantizeRgbaToEpd(rgba, width, height, options = {}) {
  const colorMode = options.colorMode === 'bw' ? 'bw' : 'bwr'
  const algorithm = options.algorithm === 'threshold' ? 'threshold' : 'dither'
  const invert = Boolean(options.invert)
  const brightness = Math.max(-80, Math.min(80, Number(options.brightness) || 0))
  const threshold = Math.max(32, Math.min(224, 128 - brightness))
  const redSensitivity = Number(options.redSensitivity ?? 1.0) || 1.0

  const w = Math.max(1, Math.round(width))
  const h = Math.max(1, Math.round(height))
  const stride = Math.ceil(w / 8)
  const planeLen = stride * h
  const bw = new Uint8Array(planeLen)
  bw.fill(0xff) // 1 = White
  const red = new Uint8Array(planeLen) // 0 = No Red
  const previewRgba = new Uint8ClampedArray(w * h * 4)

  const total = w * h
  let blackCount = 0
  let whiteCount = 0
  let redCount = 0

  if (colorMode === 'bw') {
    const work = new Float32Array(total)
    for (let i = 0; i < total; i++) {
      const a = (rgba[i * 4 + 3] ?? 255) / 255
      const r = rgba[i * 4] * a + 255 * (1 - a)
      const g = rgba[i * 4 + 1] * a + 255 * (1 - a)
      const b = rgba[i * 4 + 2] * a + 255 * (1 - a)
      work[i] = clampByte(0.299 * r + 0.587 * g + 0.114 * b + brightness)
    }

    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const idx = y * w + x
        const oldVal = work[idx]
        const cls = pickBwPixel(oldVal, algorithm === 'threshold' ? threshold : 128)
        const quantVal = cls === 1 ? 0 : 255
        if (algorithm === 'dither') {
          const err = clampErr(oldVal - quantVal)
          if (x + 1 < w) work[idx + 1] += err * (7 / 16)
          if (y + 1 < h) {
            if (x > 0) work[idx + w - 1] += err * (3 / 16)
            work[idx + w] += err * (5 / 16)
            if (x + 1 < w) work[idx + w + 1] += err * (1 / 16)
          }
        }

        const visInk = invert ? (cls === 1 ? 0 : 1) : cls
        if (cls === 1) {
          bw[y * stride + (x >> 3)] &= ~(0x80 >> (x & 7))
        }
        if (visInk === 1) {
          blackCount++
        } else {
          whiteCount++
        }
        const rgb = visInk === 1 ? EPD_PAPER_HEX.black : EPD_PAPER_HEX.white
        const po = idx * 4
        previewRgba[po] = rgb[0]
        previewRgba[po + 1] = rgb[1]
        previewRgba[po + 2] = rgb[2]
        previewRgba[po + 3] = 255
      }
    }
  } else {
    const work = new Float32Array(total * 3)
    for (let i = 0; i < total; i++) {
      const a = (rgba[i * 4 + 3] ?? 255) / 255
      work[i * 3] = clampByte(rgba[i * 4] * a + 255 * (1 - a) + brightness)
      work[i * 3 + 1] = clampByte(rgba[i * 4 + 1] * a + 255 * (1 - a) + brightness)
      work[i * 3 + 2] = clampByte(rgba[i * 4 + 2] * a + 255 * (1 - a) + brightness)
    }

    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const idx = y * w + x
        const p3 = idx * 3
        const or = clampByte(work[p3])
        const og = clampByte(work[p3 + 1])
        const ob = clampByte(work[p3 + 2])

        const cls = pickBwrPixel(or, og, ob, redSensitivity) // 0=W, 1=K, 2=R
        const qRgb = cls === 2 ? [220, 20, 20] : cls === 1 ? [0, 0, 0] : [255, 255, 255]

        if (algorithm === 'dither') {
          const er = clampErr(or - qRgb[0])
          const eg = clampErr(og - qRgb[1])
          const eb = clampErr(ob - qRgb[2])
          const diffuse = (nx, ny, wgt) => {
            if (nx < 0 || nx >= w || ny >= h) return
            const ni = (ny * w + nx) * 3
            work[ni] += er * wgt
            work[ni + 1] += eg * wgt
            work[ni + 2] += eb * wgt
          }
          diffuse(x + 1, y, 7 / 16)
          diffuse(x - 1, y + 1, 3 / 16)
          diffuse(x, y + 1, 5 / 16)
          diffuse(x + 1, y + 1, 1 / 16)
        }

        let visCls = cls
        if (invert && cls !== 2) {
          visCls = cls === 1 ? 0 : 1
        }
        const byteIdx = y * stride + (x >> 3)
        const bit = 0x80 >> (x & 7)
        if (cls === 2) {
          bw[byteIdx] |= bit
          red[byteIdx] |= bit
        } else if (cls === 1) {
          bw[byteIdx] &= ~bit
        }
        if (visCls === 2) redCount++
        else if (visCls === 1) blackCount++
        else whiteCount++

        const pr = visCls === 2 ? EPD_PAPER_HEX.red : visCls === 1 ? EPD_PAPER_HEX.black : EPD_PAPER_HEX.white
        const po = idx * 4
        previewRgba[po] = pr[0]
        previewRgba[po + 1] = pr[1]
        previewRgba[po + 2] = pr[2]
        previewRgba[po + 3] = 255
      }
    }
  }

  return {
    bw,
    red,
    previewRgba,
    stats: { width: w, height: h, blackCount, whiteCount, redCount },
  }
}

/**
 * Generate a built-in diagnostic test pattern RGBA array for quick B/W or B/W/R hardware testing.
 */
export function generateTestPatternRgba(width, height, colorMode = 'bwr') {
  const w = Math.max(1, Math.round(width))
  const h = Math.max(1, Math.round(height))
  const rgba = new Uint8ClampedArray(w * h * 4)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4
      let r = 255
      let g = 255
      let b = 255

      const inBorder = x < 4 || x >= w - 4 || y < 4 || y >= h - 4
      if (inBorder) {
        r = colorMode === 'bwr' ? 225 : 0
        g = colorMode === 'bwr' ? 15 : 0
        b = colorMode === 'bwr' ? 15 : 0
      } else if (y < h * 0.28) {
        // Top header block: red half + black half (or solid black in bw)
        if (colorMode === 'bwr' && x < w * 0.5) {
          r = 225; g = 15; b = 15
        } else {
          r = 0; g = 0; b = 0
        }
      } else if (y < h * 0.62) {
        // Middle smooth grayscale gradient ramp
        const grad = Math.round(((x - 4) / Math.max(1, w - 8)) * 255)
        r = grad; g = grad; b = grad
      } else {
        // Bottom checkerboard + red accent circle in center
        const cx = w * 0.5
        const cy = h * 0.81
        const rad = Math.min(w, h) * 0.14
        const dist = Math.hypot(x - cx, y - cy)
        if (dist <= rad) {
          r = colorMode === 'bwr' ? 225 : 0
          g = colorMode === 'bwr' ? 15 : 0
          b = colorMode === 'bwr' ? 15 : 0
        } else if ((((x >> 3) + (y >> 3)) & 1) === 0) {
          r = 0; g = 0; b = 0
        }
      }
      rgba[i] = r
      rgba[i + 1] = g
      rgba[i + 2] = b
      rgba[i + 3] = 255
    }
  }
  return rgba
}

export function buildEpdImagePayload({
  bw,
  red,
  width,
  height,
  colorMode = 'bwr',
  panel,
  rotation,
  invert,
  fullRefresh = true,
}) {
  const bwRle = encodePackBits(bw)
  const useRleBw = bwRle.length < bw.length
  let bwOut = useRleBw ? bwRle : bw
  let enc = 'raw'

  if (colorMode === 'bwr' && red) {
    const redRle = encodePackBits(red)
    if (bwRle.length + redRle.length < bw.length + red.length) {
      enc = 'rle'
      bwOut = bwRle
      return {
        width,
        height,
        color_mode: 'bwr',
        encoding: enc,
        bw_b64: bytesToBase64(bwOut),
        red_b64: bytesToBase64(redRle),
        ...(panel ? { panel } : {}),
        ...(rotation !== undefined ? { rotation } : {}),
        ...(invert !== undefined ? { invert: Boolean(invert) } : {}),
        full_refresh: Boolean(fullRefresh),
      }
    }
    return {
      width,
      height,
      color_mode: 'bwr',
      encoding: 'raw',
      bw_b64: bytesToBase64(bw),
      red_b64: bytesToBase64(red),
      ...(panel ? { panel } : {}),
      ...(rotation !== undefined ? { rotation } : {}),
      ...(invert !== undefined ? { invert: Boolean(invert) } : {}),
      full_refresh: Boolean(fullRefresh),
    }
  }

  return {
    width,
    height,
    color_mode: 'bw',
    encoding: useRleBw ? 'rle' : 'raw',
    bw_b64: bytesToBase64(bwOut),
    ...(panel ? { panel } : {}),
    ...(rotation !== undefined ? { rotation } : {}),
    ...(invert !== undefined ? { invert: Boolean(invert) } : {}),
    full_refresh: Boolean(fullRefresh),
  }
}
