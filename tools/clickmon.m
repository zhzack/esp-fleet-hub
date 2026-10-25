/* clickmon.m - macOS 系统级鼠标按键事件监听（接收侧地面真值）
 * CGEventTap listen-only：记录每一次 L/R 按下/抬起 + 毫秒时间戳。
 * 编译: clang -framework CoreGraphics -framework Foundation clickmon.m -o clickmon
 * 用法: ./clickmon  [持续秒数]   （输出到 stdout，配合重定向收集）
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <sys/time.h>

static int64_t s_seq = 0;
static int64_t s_moves = 0, s_ldown = 0, s_lup = 0, s_rdown = 0, s_rup = 0;
static CFMachPortRef s_tap = NULL;

static void ts(char *buf, size_t n)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(buf, n, "%ld.%03d", (long)(tv.tv_sec % 100000), (int)(tv.tv_usec / 1000));
}

static CGEventRef cb(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    const char *name = NULL;
    switch (type) {
        case kCGEventLeftMouseDown:  name = "L_DOWN"; s_ldown++; break;
        case kCGEventLeftMouseUp:    name = "L_UP";   s_lup++;   break;
        case kCGEventRightMouseDown: name = "R_DOWN"; s_rdown++; break;
        case kCGEventRightMouseUp:   name = "R_UP";   s_rup++;   break;
        case kCGEventMouseMoved:     s_moves++; return event;
        case kCGEventTapDisabledByTimeout:
        case kCGEventTapDisabledByUserInput:
            if (s_tap) CGEventTapEnable(s_tap, true);
            return event;
        default: return event;
    }
    s_seq++;
    CGPoint p = CGEventGetLocation(event);
    char t[32]; ts(t, sizeof(t));
    printf("%s #%lld %s (%.0f,%.0f)\n", t, s_seq, name, p.x, p.y);
    fflush(stdout);
    return event;
}

int main(int argc, char *argv[])
{
    @autoreleasepool {
        double secs = argc > 1 ? atof(argv[1]) : 30;
        CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown) |
                           CGEventMaskBit(kCGEventLeftMouseUp) |
                           CGEventMaskBit(kCGEventRightMouseDown) |
                           CGEventMaskBit(kCGEventRightMouseUp) |
                           CGEventMaskBit(kCGEventMouseMoved);
        /* kCGHIDEventTap：HID 层系统级监听，SSH 无图形会话上下文也能收到
         * （kCGSessionEventTap 在 SSH 下挂到非 GUI 会话，什么都看不到） */
        s_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionListenOnly, mask, cb, NULL);
        if (!s_tap) {
            printf("RESULT: NOT_AUTHORIZED (需在 系统设置→隐私与安全性→辅助功能 中添加本程序)\n");
            return 2;
        }
        CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, s_tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopDefaultMode);
        CGEventTapEnable(s_tap, true);
        printf("LISTENING\n"); fflush(stdout);

        /* 每秒输出一行计数摘要（moves/ldown/lup/rdown/rup），秒级对账用 */
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                         dispatch_get_main_queue());
        dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 1.0 * NSEC_PER_SEC, 0);
        dispatch_source_set_event_handler(timer, ^{
            char t[32]; ts(t, sizeof(t));
            printf("STAT %s moves=%lld ldown=%lld lup=%lld rdown=%lld rup=%lld\n",
                   t, s_moves, s_ldown, s_lup, s_rdown, s_rup);
            fflush(stdout);
        });
        dispatch_resume(timer);

        /* secs 秒后自动退出（用 GCD 定时停 runloop） */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            CFRunLoopStop(CFRunLoopGetMain());
        });
        CFRunLoopRun();
        printf("RESULT: DONE total=%lld\n", s_seq);
    }
    return 0;
}
