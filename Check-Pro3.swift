// Run from the release folder: swift Check-Pro3.swift
import Foundation
import GameController
import IOKit

func usbDevices() throws -> [String] {
    let match: [String: Any] = [
        "IOProviderClass": "IOUSBHostDevice",
        "IOPropertyMatch": ["idVendor": 0x2dc8, "idProduct": 0x6009]
    ]
    var iterator: io_iterator_t = 0
    let status = IOServiceGetMatchingServices(kIOMainPortDefault, match as CFDictionary, &iterator)
    guard status == KERN_SUCCESS else {
        throw NSError(domain: NSMachErrorDomain, code: Int(status))
    }
    defer { IOObjectRelease(iterator) }
    var names: [String] = []
    while true {
        let device = IOIteratorNext(iterator)
        if device == 0 { break }
        let property = IORegistryEntryCreateCFProperty(device, "USB Product Name" as CFString, nil, 0)
        names.append(property?.takeRetainedValue() as? String ?? "USB gamepad")
        IOObjectRelease(device)
    }
    return names.sorted()
}

func watchButtons(on controller: GCController, label: String) {
    let profile = controller.physicalInputProfile
    for button in profile.allButtons {
        let aliases = button.aliases.sorted()
        let name = aliases.isEmpty ? (button.localizedName ?? "Unnamed button") : aliases.joined(separator: " / ")
        button.valueChangedHandler = { _, value, pressed in
            let level = String(format: "%.3f", value)
            print("[\(label)] \(name)  \(pressed ? "pressed" : "released")  \(level)")
        }
    }
    print("  Button names: \(profile.buttons.keys.sorted().joined(separator: ", "))")
}

let arguments = Array(CommandLine.arguments.dropFirst())
guard arguments.count <= 1, let seconds = Int(arguments.first ?? "20"), (1...300).contains(seconds) else {
    print("Usage: swift Check-Pro3.swift [1–300 seconds]")
    exit(2)
}

print("Watching USB identity 2dc8:6009 and Apple's game-controller list for \(seconds) s.")
print("Press and release A on a Frame controller. Then try a grip or an analog trigger.")
print("Button events are labeled by controller. Disconnect other gamepads if their names are ambiguous.")
GCController.shouldMonitorBackgroundEvents = true
var previousUSB: String?
var previousCount: Int?
var monitoring: [ObjectIdentifier: GCController] = [:]
let deadline = Date().addingTimeInterval(TimeInterval(seconds))
while Date() < deadline {
    let usb: String
    do {
        let names = try usbDevices()
        usb = names.isEmpty ? "absent" : names.joined(separator: ", ")
    } catch {
        usb = "query failed (\(error.localizedDescription))"
    }
    if usb != previousUSB { print("USB 2dc8:6009: \(usb)"); previousUSB = usb }

    let controllers = GCController.controllers()
    if previousCount != controllers.count {
        print("Apple game controllers: \(controllers.count)")
        previousCount = controllers.count
    }
    let connected = Set(controllers.map(ObjectIdentifier.init))
    monitoring = monitoring.filter { connected.contains($0.key) }
    for controller in controllers where monitoring[ObjectIdentifier(controller)] == nil {
        let label = "\(controller.vendorName ?? "Unnamed controller") (\(controller.productCategory))"
        print("  Controller: \(label)")
        monitoring[ObjectIdentifier(controller)] = controller
        watchButtons(on: controller, label: label)
    }
    RunLoop.main.run(until: min(deadline, Date().addingTimeInterval(0.5)))
}
