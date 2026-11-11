/*
 * A "baseband" entry in the device tree plane (boot-arg cell=1).
 *
 * iOS calls a device cellular by its device tree: MobileGestalt's HasBaseband and cellular-data, and with them Settings'
 * Cellular pane, look for IODeviceTree:/baseband. The OnePlus 8 Pro port has its loader add the node to the tree it hands
 * the kernel (at the root and in arm-io: which of the two is read was not known there); here the loader is not touched,
 * and the kernel adds the two registry entries itself before user space starts. They carry a name and nothing else: no
 * driver matches on them, and the modem is instantnoodle_cell.c's.
 */
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>

extern "C" void hk_cell_dt_baseband(void);

static int
add_under(const char *path)
{
	IORegistryEntry *parent = IORegistryEntry::fromPath(path, gIODTPlane);
	IORegistryEntry *entry;
	int result = 0;

	if (parent == NULL) {
		return 0;
	}
	entry = parent->childFromPath("baseband", gIODTPlane);
	if (entry != NULL) {
		entry->release();
		parent->release();
		return 2;
	}
	entry = new IORegistryEntry;
	if (entry != NULL && entry->init()) {
		OSData *name = OSData::withBytes("baseband", 9), *compatible = OSData::withBytes("baseband,hackios", 17);

		entry->setName("baseband");
		if (name != NULL) {
			entry->setProperty("name", name);
			entry->setProperty("device_type", name);
			name->release();
		}
		if (compatible != NULL) {
			entry->setProperty("compatible", compatible);
			compatible->release();
		}
		result = entry->attachToParent(parent, gIODTPlane) ? 1 : 0;
	}
	if (entry != NULL) {
		entry->release();
	}
	parent->release();
	return result;
}

void
hk_cell_dt_baseband(void)
{
	static const char *const said[3] = { "NOT made", "made", "there already" };
	int root = add_under("/"), arm = add_under("/arm-io");

	IOLog("cell: device tree entry \"baseband\": at the root %s, in arm-io %s\n", said[root], said[arm]);
}
