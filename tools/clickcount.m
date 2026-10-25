/* clickcount.m - 系统级精确点击计数（免权限对账用）
 * CGEventSourceCounterForEventType 读取 HID 系统状态的累计事件计数，
 * 起止两次读取的差值 = 窗口内真实收到的按下/抬起次数（无轮询漏计）。
 * 编译: clang -framework CoreGraphics -framework Foundation clickcount.m -o clickcount
 * 用法: ./clickcount [窗口秒数，默认 45]；每秒打印一次增量。
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

int main(int argc, char *argv[])
{
    @autoreleasepool {
        double secs = argc > 1 ? atof(argv[1]) : 45;
        long d0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseDown);
        long u0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseUp);
        long m0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventMouseMoved);
        long k0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown);
        long ku0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyUp);
        long f0 = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventFlagsChanged);
        printf("BASE down=%ld up=%ld moves=%ld keydown=%ld keyup=%ld flags=%ld\n", d0, u0, m0, k0, ku0, f0); fflush(stdout);

        NSDate *start = [NSDate date];
        while (-[start timeIntervalSinceNow] < secs) {
            usleep(1000000);
            long d = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseDown);
            long u = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseUp);
            long m = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventMouseMoved);
            long k = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown);
            long ku = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyUp);
            long f = CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventFlagsChanged);
            printf("STAT +down=%ld +up=%ld +moves=%ld +keydown=%ld +keyup=%ld +flags=%ld\n",
                   d - d0, u - u0, m - m0, k - k0, ku - ku0, f - f0);
            fflush(stdout);
        }
        printf("FINAL +down=%ld +up=%ld +moves=%ld +keydown=%ld +keyup=%ld +flags=%ld\n",
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseDown) - d0,
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventLeftMouseUp) - u0,
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventMouseMoved) - m0,
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyDown) - k0,
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventKeyUp) - ku0,
               CGEventSourceCounterForEventType(kCGEventSourceStateHIDSystemState, kCGEventFlagsChanged) - f0);
    }
    return 0;
}
