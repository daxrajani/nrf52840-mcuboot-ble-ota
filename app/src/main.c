/*
 * Secure A/B OTA reference firmware (nRF52840 DK).
 *
 * - BLE peripheral + SMP (MCUmgr) for image upload to slot 1
 * - Confirms the running image after BLE comes up successfully
 */

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#if defined(NRF_POWER)
#include <helpers/nrfx_reset_reason.h>
#endif
#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS) && defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#endif
#if defined(CONFIG_MCUMGR_GRP_OS_RESET_HOOK)
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#endif

LOG_MODULE_REGISTER(secure_ble_ota, LOG_LEVEL_INF);

static unsigned int tick_ms(void)
{
	return (unsigned int)k_uptime_get_32();
}

#if defined(NRF_POWER)
/* Nordic register is authoritative; see Product Specification RESETREAS (cumulative if not cleared) */
static void log_nordic_resetreas_register(void)
{
	const uint32_t r = nrfx_reset_reason_get();

	LOG_INF("Nordic nRF RESETREAS (nrfx) raw=0x%08" PRIx32, r);
	if (r & (uint32_t)NRFX_RESET_REASON_RESETPIN_MASK) {
		LOG_WRN("  - RESETPIN: nRESET / pin reset (includes debugger, wiring, some EMI on nRESET)");
	}
	if (r & (uint32_t)NRFX_RESET_REASON_SREQ_MASK) {
		LOG_INF("  - SREQ: soft reset (NVIC, sys_reboot, reset mgmt, etc.)");
	}
	if (r & (uint32_t)NRFX_RESET_REASON_DOG_MASK) {
		LOG_INF("  - DOG: watchdog");
	}
	if (r & (uint32_t)NRFX_RESET_REASON_LOCKUP_MASK) {
		LOG_INF("  - LOCKUP: CPU lockup / fault class");
	}
	if (r & (uint32_t)NRFX_RESET_REASON_OFF_MASK) {
		LOG_INF("  - OFF: system off wake (check PS for your silicon)");
	}
	if (r == 0U) {
		LOG_INF("  (no source bits: often POR/BOR on-chip generator — see nRF reset docs)");
	}
}
#endif

#if IS_ENABLED(CONFIG_HWINFO)
static void log_hw_reset_cause(void)
{
	uint32_t cause = 0;
	int err = hwinfo_get_reset_cause(&cause);

	if (err != 0) {
		LOG_WRN("hwinfo_get_reset_cause failed: %d", err);
		return;
	}

	LOG_INF("Reset cause (hwinfo abstracted) raw=0x%08" PRIx32, cause);
	if (cause & RESET_PIN) {
		LOG_INF("  bit: external reset pin");
	}
	if (cause & RESET_SOFTWARE) {
		LOG_INF("  bit: software reset (sys_reboot / os_mgmt / etc.)");
	}
	if (cause & RESET_BROWNOUT) {
		LOG_INF("  bit: brownout");
	}
	if (cause & RESET_POR) {
		LOG_INF("  bit: power-on reset");
	}
	if (cause & RESET_WATCHDOG) {
		LOG_INF("  bit: watchdog");
	}
	if (cause & RESET_DEBUG) {
		LOG_INF("  bit: debug");
	}
	if (cause & RESET_CPU_LOCKUP) {
		LOG_INF("  bit: CPU lockup (fault/hardfault family)");
	}
	if (cause == 0U) {
		LOG_INF("  (no standard flags set — see SoC docs if unexpected)");
	}
}
#endif

#ifndef APP_FIRMWARE_VERSION
#define APP_FIRMWARE_VERSION "0.0.0-dev"
#endif

#ifndef APP_BLE_NAME
#define APP_BLE_NAME "Dax_BLE_v1"
#endif

/* Put name in primary ADV for better phone scanner name visibility. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, APP_BLE_NAME, sizeof(APP_BLE_NAME) - 1),
};

/* SMP service UUID — must match Zephyr SMP BT GATT service (SMP_BT_SVC_UUID_VAL). */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
};

static int start_advertising(void);
static void adv_restart_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

/* Count of active ACL links (do not run boot_write_img_confirmed while > 0). */
static atomic_t ble_active_conns = ATOMIC_INIT(0);

static void confirm_dwork_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(confirm_dwork, confirm_dwork_handler);

static int start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		return 0;
	}

	return err;
}

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int err = start_advertising();

	if (err) {
		LOG_ERR("Advertising restart failed: %d", err);
	} else {
		LOG_INF("Advertising restarted (connectable again)");
	}
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("t=%u ms: connection failed (0x%02x)", tick_ms(), err);
		(void)k_work_submit(&adv_restart_work);
		return;
	}

	char addr[BT_ADDR_LE_STR_LEN];
	struct bt_conn_info info;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	(void)atomic_inc(&ble_active_conns);
	if (bt_conn_get_info(conn, &info) == 0) {
		LOG_INF("t=%u ms: connected %s (active_conns=%d) int=%u lat=%u to=%u",
			tick_ms(), addr, (int)atomic_get(&ble_active_conns), info.le.interval,
			info.le.latency, info.le.timeout);
	} else {
		LOG_INF("t=%u ms: connected %s (active_conns=%d) (no conn info)", tick_ms(), addr,
			(int)atomic_get(&ble_active_conns));
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (atomic_get(&ble_active_conns) > 0) {
		(void)atomic_dec(&ble_active_conns);
	} else {
		LOG_WRN("t=%u ms: disconnect but active_conns was 0 (bug?)", tick_ms());
	}
	LOG_INF("t=%u ms: disconnected %s HCI=0x%02x (active_conns=%d)", tick_ms(), addr, reason,
		(int)atomic_get(&ble_active_conns));
	(void)k_work_submit(&adv_restart_work);
	/* When the link drops, it is safe to (re)try image confirm off the air. */
	(void)k_work_reschedule(&confirm_dwork, K_MSEC(1000));
	LOG_INF("t=%u ms: image confirm (if needed) re-scheduled 1s after disconnect", tick_ms());
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err se_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("t=%u ms: security %s level=%u se_err=%d", tick_ms(), addr, (unsigned)level,
		(int)se_err);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};

static void confirm_running_image(void)
{
	/*
	 * mcuboot_swap_type() returns BOOT_SWAP_TYPE_REVERT when this boot is a
	 * test swap (copy_done=SET, image_ok=UNSET in the primary trailer). Only in
	 * that state does boot_write_img_confirmed() actually write to flash; for all
	 * other states it is a documented no-op that returns 0.
	 */
	int swap_type = mcuboot_swap_type();

	if (swap_type != BOOT_SWAP_TYPE_REVERT) {
		LOG_INF("t=%u ms: image confirm skipped — swap_type=%d "
			"(BOOT_SWAP_TYPE_REVERT=4 needed; this is a normal boot, no pending revert)",
			tick_ms(), swap_type);
		return;
	}

	LOG_INF("t=%u ms: test swap detected — boot_write_img_confirmed() writing image_ok flag",
		tick_ms());
	printk("secure_ble_ota: confirming test image (swap_type=REVERT)\n");

	int err = boot_write_img_confirmed();

	if (err) {
		LOG_ERR("t=%u ms: boot_write_img_confirmed() FAILED: %d — MCUboot will revert!",
			tick_ms(), err);
		printk("secure_ble_ota: ERROR boot_write_img_confirmed() failed: %d\n", err);
	} else {
		LOG_INF("t=%u ms: image confirmed — MCUboot will not revert this slot on next boot",
			tick_ms());
		printk("secure_ble_ota: image confirmed OK\n");
	}
}

/* Defer image confirmation: flash I/O in bt_ready can starve the LL during the first
 * connection window and is a common source of "instant" disconnects on phones.
 */

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS) && defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
/* True while an image slot-1 upload is in progress (prevents boot flash + OTA flash overlap). */
static volatile bool img_dfu_active;
/* True after DFU_PENDING: upload done, pending flag set, reset from phone is expected. */
static volatile bool img_dfu_pending;

static enum mgmt_cb_return img_dfu_status_cb(uint32_t event, enum mgmt_cb_return prev_status,
					      int32_t *rc, uint16_t *group, bool *abort_more,
					      void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (event == MGMT_EVT_OP_IMG_MGMT_DFU_STARTED) {
		img_dfu_active = true;
		img_dfu_pending = false;
		int wst = k_work_cancel_delayable(&confirm_dwork);

		printk("secure_ble_ota: DFU_STARTED — slot-1 upload begun\n");
		LOG_INF("t=%u ms: MCUmgr img DFU_STARTED (off=0 chunk); img_dfu_active=1; "
			"k_work_cancel_delayable -> busy_flags=0x%x",
			tick_ms(), wst);
	} else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING) {
		img_dfu_active = false;
		img_dfu_pending = true;
		(void)k_work_reschedule(&confirm_dwork, K_MSEC(3000));
		printk("secure_ble_ota: DFU_PENDING — slot-1 upload complete; image test-pending set\n");
		LOG_INF("t=%u ms: MCUmgr img DFU_PENDING (upload to slot-1 done); "
			"img_dfu_pending=1; confirm re-sched in 3s",
			tick_ms());
	} else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED) {
		img_dfu_active = false;
		img_dfu_pending = false;
		(void)k_work_reschedule(&confirm_dwork, K_MSEC(3000));
		printk("secure_ble_ota: DFU_STOPPED — upload aborted or error\n");
		LOG_INF("t=%u ms: MCUmgr img DFU_STOPPED (error/reset path); "
			"img_dfu_active=0; confirm re-sched in 3s",
			tick_ms());
	} else {
		LOG_WRN("t=%u ms: unexpected MCUmgr image evt=0x%08" PRIx32, tick_ms(), event);
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback img_dfu_mcb = {
	.callback = img_dfu_status_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED | MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
		    MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED,
};
#endif

#if defined(CONFIG_MCUMGR_GRP_OS_RESET_HOOK)
static enum mgmt_cb_return os_reset_request_cb(uint32_t event, enum mgmt_cb_return prev_status,
					       int32_t *rc, uint16_t *group, bool *abort_more,
					       void *data, size_t data_size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS) && defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
	/*
	 * Some phone apps (nRF Connect iOS/Android) send an OS reset command
	 * immediately after connecting, before the image upload has started.
	 * Block that early reset so the upload can proceed; allow reset only
	 * after DFU_PENDING (upload complete) or DFU_STOPPED (error).
	 */
	if (!img_dfu_active && !img_dfu_pending &&
	    mcuboot_swap_type() != BOOT_SWAP_TYPE_TEST) {
		/* Block resets that arrive before any upload has started (anti-early-reset).
		 * Allow if: upload active, upload just completed, OR slot-1 already has a
		 * test image pending (e.g. downgrade: nRF Connect re-marks existing slot-1
		 * image via state_write without re-uploading — no DFU events fire). */
		bool force = false;

		if (data != NULL && data_size >= sizeof(struct os_mgmt_reset_data)) {
			const struct os_mgmt_reset_data *rd = data;
			force = rd->force;
		}

		if (!force) {
			printk("secure_ble_ota: OS RESET blocked — no DFU active (anti-early-reset)\n");
			LOG_WRN("t=%u ms: MCUmgr os reset blocked (no DFU active, force=0); "
				"phone app sent premature reset — ignoring",
				tick_ms());
			*rc = MGMT_ERR_EBUSY;
			return MGMT_CB_ERROR_RC;
		}
	}
#endif

	/* printk: synchronous; LOG deferred mode could omit this line before warm reboot. */
	if (data != NULL && data_size >= sizeof(struct os_mgmt_reset_data)) {
		const struct os_mgmt_reset_data *rd = data;

		printk("secure_ble_ota: MCUmgr OS RESET (SMP) force=%d delay_ms=%d -> warm reboot (SREQ)\n",
		       (int)rd->force, CONFIG_MCUMGR_GRP_OS_RESET_MS);
		LOG_INF("t=%u ms: MCUmgr os reset (SMP); force=%d; warm reboot in %d ms",
			tick_ms(), (int)rd->force, CONFIG_MCUMGR_GRP_OS_RESET_MS);
	} else {
		printk("secure_ble_ota: MCUmgr OS RESET (SMP) no payload\n");
		LOG_INF("t=%u ms: MCUmgr os reset (no payload details)", tick_ms());
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback os_reset_mcb = {
	.callback = os_reset_request_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
#endif

static void confirm_dwork_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_INF("t=%u ms: confirm_dwork running (ble_active_conns=%d)", tick_ms(),
		(int)atomic_get(&ble_active_conns));

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS) && defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
	if (img_dfu_active) {
		/* Slot 1 is actively being written — defer to avoid concurrent flash I/O. */
		LOG_INF("t=%u ms: deferring confirm: slot-1 OTA write active", tick_ms());
		(void)k_work_reschedule(&confirm_dwork, K_SECONDS(1));
		return;
	}
#endif

	/*
	 * Do NOT defer on ble_active_conns > 0.
	 *
	 * After an MCUboot test swap the nRF Connect DFU app reconnects
	 * immediately to verify the new version.  If we defer while BLE is
	 * connected the new image is never confirmed and MCUboot reverts it
	 * on the next boot — the OTA silently fails.
	 *
	 * The nRF52840 SoftDevice Controller handles flash arbitration in
	 * hardware; a single-byte image_ok write is safe while radio is active.
	 * The 2-second initial delay (scheduled in bt_ready) already protects
	 * the critical first-connection window.
	 */
	confirm_running_image();
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!conn) {
		return;
	}
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_WRN("Pairing cancelled: %s", addr);
}

static struct bt_conn_auth_cb auth_cb = {
	.cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!conn) {
		return;
	}
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing complete: %s (bonded=%d)", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	if (conn) {
		char addr[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
		LOG_ERR("Pairing failed: %s reason=%d", addr, (int)reason);
	} else {
		LOG_ERR("Pairing failed: reason=%d", (int)reason);
	}
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		int serr = settings_load();

		if (serr) {
			LOG_WRN("settings_load() failed: %d (pairing/identity not restored)", serr);
		} else {
			LOG_INF("Bluetooth settings (NVS) loaded (any earlier 'No ID address' is before this)");
		}
	}

	LOG_INF("Bluetooth initialized (SMP transport is registered by MCUmgr)");

	err = bt_conn_auth_cb_register(&auth_cb);
	if (err) {
		LOG_ERR("bt_conn_auth_cb_register failed: %d", err);
		return;
	}
	err = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (err) {
		LOG_ERR("bt_conn_auth_info_cb_register failed: %d", err);
		return;
	}
	bt_conn_cb_register(&conn_callbacks);

	err = bt_set_name(APP_BLE_NAME);
	if (err) {
		LOG_ERR("Set BLE name failed (%d)", err);
		return;
	}

	err = start_advertising();
	if (err) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}

	LOG_INF("Advertising as '%s' (SMP / image upload enabled)", APP_BLE_NAME);
	/* First confirm attempt: 2 s delay avoids flash I/O in the first BLE connection window.
	 * Only deferred further if a slot-1 DFU write is actively in progress. */
	(void)k_work_schedule(&confirm_dwork, K_MSEC(2000));
	LOG_INF("t=%u ms: image confirm work scheduled in 2s (deferred only if slot-1 DFU active)",
		tick_ms());
}

int main(void)
{
#if defined(NRF_POWER)
	log_nordic_resetreas_register();
#endif
#if IS_ENABLED(CONFIG_HWINFO)
	log_hw_reset_cause();
	/* Nordic RESETREAS is cumulative; clear so the *next* reset only reflects that event. */
	(void)hwinfo_clear_reset_cause();
	LOG_INF("SoC reset-reason register cleared; next boot shows only the latest reset type");
#endif
	LOG_INF("MCUboot-aware firmware v%s", APP_FIRMWARE_VERSION);
	LOG_INF("DFU: nRF Connect finishes the upload, then the kit resets; MCUboot swaps the new "
	       "image from slot 1 (one or two MCUboot lines on UART is normal).");

#ifdef CRASH_DEMO
	LOG_ERR("CRASH_DEMO enabled: panicking before image confirmation");
	k_panic();
#endif

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS) && defined(CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS)
	mgmt_callback_register(&img_dfu_mcb);
#endif
#if defined(CONFIG_MCUMGR_GRP_OS_RESET_HOOK)
	mgmt_callback_register(&os_reset_mcb);
#endif

	int err = bt_enable(bt_ready);

	if (err) {
		LOG_ERR("Bluetooth enable failed (%d)", err);
		return 0;
	}

	return 0;
}
