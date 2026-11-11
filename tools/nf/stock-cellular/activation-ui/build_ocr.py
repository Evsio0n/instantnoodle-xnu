"""Compile an offline, fixed-vocabulary extension of the stock Settings framebuffer OCR."""
from pathlib import Path
import json
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
K = HERE.parents[3]
DEST = K.parent / 'oneplus8-audit/stock-cellular-ui/activation-ui'
EXTRA = {
    'monitor': ['Cellular Monitor', 'CellularMonitor', 'Cellular Mon…', 'Cellular Moni…',
                'Cellular Monit…', 'Cellular Monito…',
                'GNSS Monitor'],
    'open_settings': ['Open Settings'],
    'sim_section': ['SIM and USIM'], 'gw_section': ['Primary GW'],
    'activation_section': ['Activation'], 'radio_section': ['Radio and data'],
    'live': ['Live modem'], 'stopped': ['Modem stopped'], 'waiting': ['Waiting for modem'],
    'verified': ['Verified by readback'], 'already_bound': ['Already bound'],
    'refused': ['Modem refused'], 'accepted': ['Modem accepted'],
    'submitted': ['Submitted'], 'uncertain': ['Attempt uncertain'], 'not_submitted': ['Not submitted'],
    'weather': ['Weather'], 'stocks': ['Stocks'], 'find_my': ['Find My'], 'home_app': ['Home'],
    'books': ['Books'], 'store': ['iTunes Store'], 'watch_app': ['Watch'], 'shortcuts': ['Shortcuts'],
    'files': ['Files'], 'tips': ['Tips'], 'freeform': ['Freeform'], 'utilities': ['Utilities'],
    'fitness': ['Fitness'], 'translate': ['Translate'], 'contacts_app': ['Contacts'],
    'reminders_app': ['Reminders'], 'mail': ['Mail'], 'notes': ['Notes'], 'news': ['News'],
    'app_store': ['App Store'], 'podcasts': ['Podcasts'], 'sign_in': ['Sign in to your iPhone'],
    'data_usage': ['Current Period', 'Roaming', 'Cellular Data Options'],
}
# Never interpret arbitrary OCR as data. Only the sequence number and the fixed
# vocabulary above are exported; original PNGs remain local for verification.
VALUE_RULES = '''
        let knownValues = [
            ("gw_data", ["unassigned", "mapped", "invalid mapping", "outside reported range"]),
            ("nas_data", ["nas unregistered", "nas registered", "nas searching", "nas denied"]),
            ("wds_data", ["wds disconnected", "wds connected", "wds suspended", "wds authenticating"]),
            ("dms_data", ["dms online", "dms offline", "dms low power", "dms factory test"]),
        ]
        if hit.confidence >= 0.45 {
            for (id, phrases) in knownValues {
                if let prefix = phrases.first(where: { normal == $0 || normal.hasPrefix($0+" ") }) {
                    labels.append(Label(id:id,label:prefix,box:box,confidence:hit.confidence))
                }
            }
            for (id, word, states) in [("cs_attach","cs",["attached","detached","unknown"]),
                                       ("ps_attach","ps",["attached","detached","unknown"]),
                                       ("usim_state","usim",["ready","detected","unknown","none reported","pin required","puk required"])] {
                for state in states {
                    let phrase=word+" "+state
                    if normal.range(of:"(?<![a-z])"+phrase+"(?![a-z])",options:.regularExpression) != nil {
                        labels.append(Label(id:id,label:phrase,box:box,confidence:hit.confidence))
                        break
                    }
                }
            }
            if normal.range(of:"^[0-9]{1,3} apps?[^a-z]+sim ",options:.regularExpression) != nil && normal.contains("usim") {
                labels.append(Label(id:"app_data",label:"SIM/USIM projection",box:box,confidence:hit.confidence))
            }
            if normal.hasPrefix("slot ") && normal.contains("logical") {
                labels.append(Label(id:"slot_data",label:"Physical slot projection",box:box,confidence:hit.confidence))
            }
        }
        let phaseOrDecision = ["verified","already_bound","refused","accepted","submitted","uncertain",
                               "not_submitted","live","stopped","waiting"]
        for id in phaseOrDecision {
            guard let word = vocabulary[id]?.first else { continue }
            let prefix = normalized(word)
            if normal != prefix && (normal.hasPrefix(prefix+" ·") || normal.hasPrefix(prefix+" (") ||
                normal.hasPrefix(prefix+" —") || normal.hasPrefix(prefix+" •") ||
                normal.hasPrefix(prefix+" error") || normal.hasPrefix(prefix+" QMI".lowercased())) {
                labels.append(Label(id:id,label:word,box:box,confidence:hit.confidence))
            }
        }
        if let regex = try? NSRegularExpression(pattern:"(?i)\\\\bsample\\\\s*#?\\\\s*([0-9]{1,10})\\\\b"),
           let match = regex.firstMatch(in:hit.string,range:NSRange(hit.string.startIndex...,in:hit.string)),
           let range = Range(match.range(at:1),in:hit.string), hit.confidence >= 0.45 {
            labels.append(Label(id:"sample",label:"Sample "+String(hit.string[range]),box:box,confidence:hit.confidence))
        }
'''
def main():
    source = (HERE.parent/'screen.swift').read_text()
    # The inherited vocabulary already has Cellular Data Options for Settings panes.
    extra = {k:v for k,v in EXTRA.items()}
    extra['data_usage'] = ['Current Period', 'Roaming']
    anchor = 'let vocabulary = [\n'
    code = ',\n'.join(json.dumps(k)+':'+json.dumps(v,ensure_ascii=False) for k,v in extra.items())
    assert source.count(anchor)==1
    source = source.replace(anchor,anchor+code+',\n')
    anchor = '        if let (id,text)=lookup[normal]'
    assert source.count(anchor)==1
    source = source.replace(anchor,VALUE_RULES+anchor)
    source = source.replace('let blocked = [','let blocked = ["enable cellular data","sim locked","enter pin","activate sim","carrier settings update","continue","allow","ok",',1)
    DEST.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as name:
        swift = Path(name)/'screen.swift'
        swift.write_text(source)
        subprocess.run(['swiftc','-O',str(swift),'-o',str(DEST/'screen')],check=True)
if __name__=='__main__':main()
