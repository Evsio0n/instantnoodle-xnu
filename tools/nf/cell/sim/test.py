"""Compile the cell=1 production UIM decoder and refresh path with in-memory QRTR replies."""
from pathlib import Path
import hashlib
import subprocess
import tempfile

here=Path(__file__).resolve().parent
driver=here.parent/'instantnoodle_cell.c'
source=driver.read_text()
model=source[source.index('enum cell_sim_kind {'):source.index('static bool option_sms_self;')]
query=source[source.index('static bool\nuim_card_status(void)'):source.index('static int\nnas_registration(')]
owner=source[source.index('static void\nmodem_bringup(bool initial)'):source.index('/* ---- the service the rest')]
with tempfile.TemporaryDirectory() as temp:
    tmp=Path(temp)
    (tmp/'model.inc').write_text(model)
    (tmp/'query.inc').write_text(query)
    (tmp/'owner.inc').write_text(owner)
    start=source.index('uint32_t uim_node, uim_port;')
    end=source.index('uint32_t voice_node, voice_port;',start)
    indication=source[start:source.rindex('{',start,end)]
    (tmp/'indication.inc').write_text(indication)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I',str(tmp),str(here/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
    subprocess.run(['python3',str(here.parent/'port.py'),str(here.parent/'ref-pro'),str(tmp/'ported')],check=True)
    assert (tmp/'ported/instantnoodle_cell.c').read_bytes()==driver.read_bytes()
print('same generated source; patch SHA256',hashlib.sha256((here/'sim-state.patch').read_bytes()).hexdigest())
