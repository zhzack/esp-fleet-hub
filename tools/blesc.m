/* blesc.m - macOS BLE 配对诊断工具
 * 扫描 esp32_001 -> 连接 -> 发现服务 -> 订阅 HID Input Report (0x2A4D)
 * 订阅需要加密链路，SMP 配对由系统自动触发；各阶段回调打印，定位卡点。
 * 编译: clang -framework Foundation -framework CoreBluetooth blesc.m -o blesc
 * 用法: ./blesc [名字] [超时秒]
 */
#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>

static NSTimeInterval kTimeout;
static NSDate *start;

@interface Delegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (strong) CBCentralManager *cm;
@property (strong) CBPeripheral *target;
@property (strong) NSString *want;
@end

static void finish(NSString *msg) {
    printf("RESULT: %s\n", msg.UTF8String);
    fflush(stdout);
    exit(0);
}

@implementation Delegate
- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    printf("STATE: %ld\n", (long)central.state);
    fflush(stdout);
    if (central.state == CBManagerStateUnauthorized)
        finish(@"unauthorized — need Bluetooth permission (Privacy & Security -> Bluetooth)");
    if (central.state == CBManagerStatePoweredOn) {
        NSLog(@"scanning for %@ ...", self.want);
        [central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
    } else if (central.state != CBManagerStateUnknown) {
        finish([NSString stringWithFormat:@"bt not usable: %ld", (long)central.state]);
    }
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary *)adv
                  RSSI:(NSNumber *)rssi {
    NSString *name = peripheral.name ?: adv[CBAdvertisementDataLocalNameKey];
    if (![name isEqualToString:self.want]) return;
    printf("FOUND: %s rssi=%ld identifier=%s\n", name.UTF8String, rssi.longValue,
           peripheral.identifier.UUIDString.UTF8String);
    fflush(stdout);
    self.target = peripheral;
    peripheral.delegate = self;
    [central stopScan];
    [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral {
    printf("CONNECTED: %s\n", peripheral.identifier.UUIDString.UTF8String);
    fflush(stdout);
    [peripheral discoverServices:@[[CBUUID UUIDWithString:@"1812"]]];
}

- (void)centralManager:(CBCentralManager *)central
   didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error {
    finish([NSString stringWithFormat:@"connect FAILED: %@", error]);
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error {
    if (error) finish([NSString stringWithFormat:@"discoverServices error: %@", error]);
    for (CBService *s in peripheral.services)
        printf("SERVICE: %s\n", s.UUID.UUIDString.UTF8String), fflush(stdout);
    [peripheral discoverCharacteristics:nil forService:peripheral.services.firstObject];
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error {
    if (error) finish([NSString stringWithFormat:@"discoverCharacteristics error: %@", error]);
    for (CBCharacteristic *c in service.characteristics) {
        printf("CHAR: %s props=0x%lx\n", c.UUID.UUIDString.UTF8String, (unsigned long)c.properties);
        fflush(stdout);
        if ([c.UUID.UUIDString isEqualToString:@"2A4D"] && (c.properties & CBCharacteristicPropertyNotify)) {
            printf("SUBSCRIBE: %s (需要加密链路，系统配对应在此触发)\n", c.UUID.UUIDString.UTF8String);
            fflush(stdout);
            [peripheral setNotifyValue:YES forCharacteristic:c];
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error {
    if (error)
        finish([NSString stringWithFormat:@"SUBSCRIBE FAILED: %s (SMP/pairing 在此失败) %@",
                characteristic.UUID.UUIDString.UTF8String, error]);
    finish([NSString stringWithFormat:@"SUBSCRIBED OK to %s — 已配对并加密成功",
            characteristic.UUID.UUIDString.UTF8String]);
}

- (void)peripheral:(CBPeripheral *)peripheral didReadRSSI:(NSNumber *)RSSI error:(NSError *)error {}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        Delegate *d = [Delegate new];
        d.want = argc > 1 ? @(argv[1]) : @"esp32_001";
        kTimeout = argc > 2 ? atof(argv[2]) : 40;
        start = [NSDate date];
        d.cm = [[CBCentralManager alloc] initWithDelegate:d queue:nil];
        while (-[start timeIntervalSinceNow] > -kTimeout) {
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
            if (d.target && d.target.state == CBPeripheralStateConnected && d.target.services.count == 0) {
                // 已连接但服务发现未完成，继续等回调
            }
        }
        finish(@"TIMEOUT — 见上方最后一条输出");
    }
    return 0;
}
