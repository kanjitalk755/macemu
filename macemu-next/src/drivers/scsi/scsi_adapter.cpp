/*
 *  scsi_adapter.cpp - SCSI adapter layer
 *
 *  Thin adapter that defers to g_platform function pointers.
 *  Replaces scsi_dummy.cpp - no changes to core code needed.
 */

#include "sysdeps.h"
#include "platform.h"
#include "scsi.h"

/*
 *  Initialization
 */
void SCSIInit(void)
{
	g_platform.scsi_init();
}

/*
 *  Deinitialization
 */
void SCSIExit(void)
{
	g_platform.scsi_exit();
}

/*
 *  Set SCSI command to be sent by scsi_send_cmd()
 */
void scsi_set_cmd(int cmd_length, uint8 *cmd)
{
	g_platform.scsi_set_cmd(cmd_length, cmd);
}

/*
 *  Check for presence of SCSI target
 */
bool scsi_is_target_present(int id)
{
	return g_platform.scsi_is_target_present(id);
}

/*
 *  Set SCSI target (returns false on error)
 */
bool scsi_set_target(int id, int lun)
{
	return g_platform.scsi_set_target(id, lun);
}

/*
 *  Send SCSI command to active target
 */
bool scsi_send_cmd(size_t data_length, bool reading, int sg_index, uint8 **sg_ptr, uint32 *sg_len, uint16 *stat, uint32 timeout)
{
	return g_platform.scsi_send_cmd(data_length, reading, sg_index, sg_ptr, sg_len, stat, timeout);
}
