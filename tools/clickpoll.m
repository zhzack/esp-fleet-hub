/* clickpoll.m - 免权限的鼠标按键接收侧计数器
 * 原理：CGEventSourceSecondsSinceLastEventType 查询 HID 系统状态里
 * 左键 down/up 的最后发生时刻（公开 API，空闲检测用途，无需 TCC 授权），
 * 10ms 轮询——时刻回退即新事件。精度 ±10ms，足够做发送/接收对账。
 * 编译: clang -framework CoreGraphics -framework Foundation clickpoll.m -o clickpoll
 * 用法: ./clickpoll [持续秒数]
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <sys/time.h>

static void ts(char *buf, size_t n)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(buf, n, "%ld.%03d", (long)(tv.tv_sec % 100000), (int)(tv.tv_usec / 1000));
}

int main(int argc, char *argv[])
{
    @autoreleasepool {
        double secs = argc > 1 ? atof(argv[1]) : 60;
        long nDown = 0, nUp = 0;
        double prevDownAge = 1e9, prevUpAge = 1e9;
        NSDate *start = [NSDate date];
        char t[32];

        printf("POLLING\n"); fflush(stdout);
        while (-[start timeIntervalSinceNow] > -secs) {
            double dAge = CGEventSourceSecondsSinceLastEventType(
                kCGEventSourceStateHIDSystemState, kCGEventLeftMouseDown);
            double uAge = CGEventSourceSecondsSinceLastEventType(
                kCGEventSourceStateHIDSystemState, kCGEventLeftMouseUp);
            if (dAge < prevDownAge - 0.002) {
                nDown++; ts(t, sizeof(t));
                printf("%s L_DOWN #%ld\n", t, nDown); fflush(stdout);
            }
            if (uAge < prevUpAge - 0.002) {
                nUp++; ts(t, sizeof(t));
                printf("%s L_UP #%ld\n", t, nUp); fflush(stdout);
            }
            prevDownAge = dAge;
            prevUpAge = uAge;
            usleep(10000);
        }
        printf("RESULT down=%ld up=%ld\n", nDown, nUp);
    }
    return 0;
}
