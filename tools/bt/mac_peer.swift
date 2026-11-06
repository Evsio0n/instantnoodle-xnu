// tools/bt/mac_peer.swift [SECONDS]: this Mac as the controlled BLE peer of userland/src/bt_scan.m (docs/TESTING.md's Linux bluetoothctl fixture, for a Mac):
// it advertises the test service, and its characteristic takes reads and writes with no pairing. It changes no setting of the Mac and ends by itself.
// swiftc -o out/nf-mac-peer tools/bt/mac_peer.swift
import CoreBluetooth
import Foundation

let serviceUUID = CBUUID(string: "86DBFDC4-50F4-4D5C-BD34-9672E9EF58F2")
let valueUUID = CBUUID(string: "86DBFDC4-50F4-4D5C-BD34-9672E9EF58F3")

final class Peer: NSObject, CBPeripheralManagerDelegate {
    var manager: CBPeripheralManager!
    var value = Data(repeating: 0, count: 32)
    var characteristic: CBMutableCharacteristic!

    override init() {
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        print("mac_peer: state \(peripheral.state.rawValue), authorization \(CBManager.authorization.rawValue)")
        guard peripheral.state == .poweredOn else { return }
        characteristic = CBMutableCharacteristic(type: valueUUID, properties: [.read, .write], value: nil, permissions: [.readable, .writeable])
        let service = CBMutableService(type: serviceUUID, primary: true)
        service.characteristics = [characteristic]
        peripheral.add(service)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        print("mac_peer: service added, error \(String(describing: error))")
        peripheral.startAdvertising([CBAdvertisementDataServiceUUIDsKey: [serviceUUID], CBAdvertisementDataLocalNameKey: "HKBT-test"])
    }

    func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        print("mac_peer: advertising, error \(String(describing: error))")
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        print("mac_peer: read at \(request.offset)")
        request.value = request.offset < value.count ? value.subdata(in: request.offset..<value.count) : Data()
        peripheral.respond(to: request, withResult: .success)
    }

    func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        for request in requests {
            print("mac_peer: write of \(request.value?.count ?? 0) bytes")
            value = request.value ?? Data()
        }
        peripheral.respond(to: requests[0], withResult: .success)
    }
}

let seconds = CommandLine.arguments.count > 1 ? Double(CommandLine.arguments[1]) ?? 300 : 300
setvbuf(stdout, nil, _IOLBF, 0)
let peer = Peer()
RunLoop.main.run(until: Date(timeIntervalSinceNow: seconds))
peer.manager.stopAdvertising()
print("mac_peer: done")
