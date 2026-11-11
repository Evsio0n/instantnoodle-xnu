import Foundation
import ImageIO
import Vision

// Only exact, approved navigation labels leave this process. No image pixels or raw OCR strings are logged.
struct Label: Encodable {
    let id: String
    let label: String
    let box: [Int]
    let confidence: Float
}
struct Receipt: Encodable {
    let width: Int
    let height: Int
    let labels: [Label]
}
let vocabulary = [
"settings":["Settings", "設定", "设置"],
"cellular":["Cellular", "Mobile Data", "Mobile Service", "Cellular Data", "行動服務", "行動數據", "流動網絡", "蜂窝网络", "移动网络"],
"airplane":["Airplane Mode", "飛航模式", "飞行模式"],
"wifi":["Wi-Fi", "WLAN"],
"bt":["Bluetooth", "藍牙", "蓝牙"],
"vpn":["VPN"],
"hotspot":["Personal Hotspot", "個人熱點"],
"notifications":["Notifications", "通知"],
"sounds":["Sounds", "Sounds & Haptics"],
"general":["General", "一般"],
"display":["Display & Brightness"],
"focus":["Focus"],
"screen_time":["Screen Time"],
"facetime":["FaceTime"],
"calendar":["Calendar"],
"photos":["Photos"],
"maps":["Maps"],
"clock":["Clock"],
"wallet":["Wallet"],
"health":["Health"],
"battery":["Battery"],
"sims":["SIMs", "SIM Applications"],
"network":["Network Selection", "Cellular Data Options", "Mobile Data Options", "No SIM", "Add eSIM"],
"search":["Search", "搜尋"],
"carrier":["Carrier Services"],
"root_privacy":["Privacy & Security"],
"cell_pane":["Cellular Plans", "Cellular Usage", "Network Provider Services", "SIM PIN"],
"update":["Software Update"],
"passcode":["Passcode"]
]
func normalized(_ input: String) -> String {
    input.precomposedStringWithCompatibilityMapping.lowercased().replacingOccurrences(of:"’",with:"'")
        .replacingOccurrences(of:"‑",with:"-").replacingOccurrences(of:"–",with:"-")
        .trimmingCharacters(in:CharacterSet(charactersIn:" ‹›><〈〉"))
        .split(whereSeparator:{$0.isWhitespace}).joined(separator:" ")
}
let allowed = vocabulary.flatMap { id, labels in labels.map { (normalized($0), (id,$0)) } }
let lookup = Dictionary(uniqueKeysWithValues: allowed)
let blocked = ["allow while using app","allow once","don't allow","do not allow","allow once","while using the app",
    "turn on location services to","determine your location","allow access to your location","use your location",
    "only this time","not now","cancel","取消","不允許","不允许","允許一次","允许一次",
    "使用 app 期間允許","使用app时允许","稍後再說","现在不要","存取您的位置","使用您的位置","确定您的位置"].map(normalized)
func extract(_ path:String) throws -> Receipt {
    let url=URL(fileURLWithPath:path)
    guard let src=CGImageSourceCreateWithURL(url as CFURL,nil), let img=CGImageSourceCreateImageAtIndex(src,0,nil) else {
        throw NSError(domain:"unreadable image",code:1)
    }
    let w=img.width,h=img.height
    let request=VNRecognizeTextRequest()
    // The English iOS desktop under test uses this host's CPU-only fast recognizer.
    // Revision 3's image-reader path can wait indefinitely on this Mac.
    request.recognitionLevel = .fast
    request.revision = VNRecognizeTextRequestRevision2
    request.usesCPUOnly = true
    request.usesLanguageCorrection = false
    request.recognitionLanguages = ["en-US"]
    try VNImageRequestHandler(cgImage:img,options:[:]).perform([request])
    var labels=[Label]()
    for ob in request.results ?? [] {
        guard let hit=ob.topCandidates(1).first else {continue}
        let normal=normalized(hit.string), r=ob.boundingBox
        let xs=max(0,Int(floor(r.minX*Double(w)))), ys=max(0,Int(floor((1-r.maxY)*Double(h))))
        let box=[xs,ys,min(w,Int(ceil(r.maxX*Double(w))))-xs,min(h,Int(ceil((1-r.minY)*Double(h))))-ys]
        if let (id,text)=lookup[normal],hit.confidence>=0.45 {
            labels.append(Label(id:id,label:text,box:box,confidence:hit.confidence))
        } else if blocked.contains(where:{normal==$0 || ($0.count>=12 && normal.contains($0))}) {
            labels.append(Label(id:"modal",label:"Modal",box:box,confidence:hit.confidence))
        }
    }
    return Receipt(width:w,height:h,labels:labels)
}
guard CommandLine.arguments.count==2 else {FileHandle.standardError.write(Data("usage: ocr image.png\n".utf8)); exit(2)}
do {
    let receipt=try extract(CommandLine.arguments[1])
    let encoder=JSONEncoder(); encoder.outputFormatting=[.sortedKeys]
    FileHandle.standardOutput.write(try encoder.encode(receipt)); print()
} catch {
    FileHandle.standardError.write(Data("image recognition failed\n".utf8)); exit(1)
}
