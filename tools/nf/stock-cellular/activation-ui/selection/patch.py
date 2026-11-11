"""Apply after the source-bound Get Selected Config integration from #285."""
from pathlib import Path

HERE = Path(__file__).resolve().parent


def insert(text, anchor, addition):
    if text.count(anchor) != 1:
        raise ValueError(f"selection observer source mismatch: {anchor}")
    return text.replace(anchor, anchor + addition)


def apply(rel, text):
    if rel.endswith("/mdm_rfs_uim.inc"):
        header = (HERE / "hk_cellular_selection.h").read_text().replace("#include <stdint.h>", "")
        text = insert(text, "static LCK_MTX_DECLARE(obs_lock,&mdm_lock_group);",
                      "\n" + header + "\n" + (HERE / "producer.inc").read_text())
        return insert(text, "lck_mtx_lock(&obs_lock);hist_phase_locked(phase);",
                      "selection_phase_locked(phase);")
    if rel.endswith("/mdm_abp_board.inc"):
        return insert(text, 'bool fields=full && !strcmp(status,"complete");', '''
    bool paired = !forced && full && t->sent && !t->watch_error && !t->send_error &&
        (fields || !strcmp(status, "ind_error"));
    selection_window(i % 2, now, paired, r);''')
    return text
