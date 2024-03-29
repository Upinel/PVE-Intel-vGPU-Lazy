// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915_reg.h"
#include "intel_cx0_phy.h"
#include "intel_cx0_reg_defs.h"
#include "intel_ddi.h"
#include "intel_ddi_buf_trans.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_hdmi.h"
#include "intel_panel.h"
#include "intel_psr.h"
#include "intel_tc.h"
#include "intel_uncore.h"

bool intel_is_c10phy(struct drm_i915_private *dev_priv, enum phy phy)
{
	if (IS_METEORLAKE(dev_priv) && (phy < PHY_C))
		return true;

	return false;
}

static void
assert_dc_off(struct drm_i915_private *i915)
{
	bool enabled;

	enabled = intel_display_power_is_enabled(i915, POWER_DOMAIN_DC_OFF);
	drm_WARN_ON(&i915->drm, !enabled);
}

static void intel_cx0_bus_reset(struct drm_i915_private *i915, enum port port, int lane)
{
	enum phy phy = intel_port_to_phy(i915, port);

	/* Bring the phy to idle. */
	intel_de_write(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
		       XELPDP_PORT_M2P_TRANSACTION_RESET);

	/* Wait for Idle Clear. */
	if (intel_de_wait_for_clear(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
				    XELPDP_PORT_M2P_TRANSACTION_RESET,
				    XELPDP_MSGBUS_TIMEOUT_SLOW)) {
		drm_err_once(&i915->drm, "Failed to bring PHY %c to idle.\n", phy_name(phy));
		return;
	}

	intel_de_write(i915, XELPDP_PORT_P2M_MSGBUS_STATUS(port, lane), ~0);
}

static int intel_cx0_wait_for_ack(struct drm_i915_private *i915, enum port port, int lane, u32 *val)
{
	enum phy phy = intel_port_to_phy(i915, port);

	if (__intel_wait_for_register(&i915->uncore,
				      XELPDP_PORT_P2M_MSGBUS_STATUS(port, lane),
				      XELPDP_PORT_P2M_RESPONSE_READY,
				      XELPDP_PORT_P2M_RESPONSE_READY,
				      XELPDP_MSGBUS_TIMEOUT_FAST_US,
				      XELPDP_MSGBUS_TIMEOUT_SLOW, val)) {
		drm_dbg_kms(&i915->drm, "PHY %c Timeout waiting for message ACK. Status: 0x%x\n", phy_name(phy), *val);
		return -ETIMEDOUT;
	}

	return 0;
}

static int __intel_cx0_read(struct drm_i915_private *i915, enum port port,
			   int lane, u16 addr, u32 *val)
{
	enum phy phy = intel_port_to_phy(i915, port);
	int ack;

	/* Wait for pending transactions.*/
	if (intel_de_wait_for_clear(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
				    XELPDP_PORT_M2P_TRANSACTION_PENDING,
				    XELPDP_MSGBUS_TIMEOUT_SLOW)) {
		drm_dbg_kms(&i915->drm, "PHY %c Timeout waiting for previous transaction to complete. Reset the bus and retry.\n", phy_name(phy));
		intel_cx0_bus_reset(i915, port, lane);
		return -ETIMEDOUT;
	}

	/* Issue the read command. */
	intel_de_write(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
		       XELPDP_PORT_M2P_TRANSACTION_PENDING |
		       XELPDP_PORT_M2P_COMMAND_READ |
		       XELPDP_PORT_M2P_ADDRESS(addr));

	/* Wait for response ready. And read response.*/
	ack = intel_cx0_wait_for_ack(i915, port, lane, val);
	if (ack < 0) {
		intel_cx0_bus_reset(i915, port, lane);
		return ack;
	}

	/* Check for error. */
	if (*val & XELPDP_PORT_P2M_ERROR_SET) {
		drm_dbg_kms(&i915->drm, "PHY %c Error occurred during read command. Status: 0x%x\n", phy_name(phy), *val);
		intel_cx0_bus_reset(i915, port, lane);
		return -EINVAL;
	}

	/* Check for Read Ack. */
	if (REG_FIELD_GET(XELPDP_PORT_P2M_COMMAND_TYPE_MASK, *val) !=
			  XELPDP_PORT_P2M_COMMAND_READ_ACK) {
		drm_dbg_kms(&i915->drm, "PHY %c Not a Read response. MSGBUS Status: 0x%x.\n", phy_name(phy), *val);
		intel_cx0_bus_reset(i915, port, lane);
		return -EINVAL;
	}

	/* Clear Response Ready flag.*/
	intel_de_write(i915, XELPDP_PORT_P2M_MSGBUS_STATUS(port, lane), ~0);

	return REG_FIELD_GET(XELPDP_PORT_P2M_DATA_MASK, *val);
}

static u8 intel_cx0_read(struct drm_i915_private *i915, enum port port,
			 int lane, u16 addr)
{
	enum phy phy = intel_port_to_phy(i915, port);
	int i, status = 0;
	u32 val;

	assert_dc_off(i915);

	for (i = 0; i < 3; i++) {
		status = __intel_cx0_read(i915, port, lane, addr, &val);

		if (status >= 0)
			break;
	}

	if (i == 3) {
		drm_err_once(&i915->drm, "PHY %c Read %04x failed after %d retries.\n", phy_name(phy), addr, i);
		return 0;
	}

	return status;
}

static int intel_cx0_wait_cwrite_ack(struct drm_i915_private *i915,
				      enum port port, int lane)
{
	enum phy phy = intel_port_to_phy(i915, port);
	int ack;
	u32 val = 0;

	/* Check for write ack. */
	ack = intel_cx0_wait_for_ack(i915, port, lane, &val);
	if (ack < 0)
		return ack;

	if ((REG_FIELD_GET(XELPDP_PORT_P2M_COMMAND_TYPE_MASK, val) !=
	     XELPDP_PORT_P2M_COMMAND_WRITE_ACK) || val & XELPDP_PORT_P2M_ERROR_SET) {
		drm_dbg_kms(&i915->drm, "PHY %c Unexpected ACK received. MSGBUS STATUS: 0x%x.\n", phy_name(phy), val);
		return -EINVAL;
	}

	return 0;
}

static int __intel_cx0_write_once(struct drm_i915_private *i915, enum port port,
				  int lane, u16 addr, u8 data, bool committed)
{
	enum phy phy = intel_port_to_phy(i915, port);

	/* Wait for pending transactions.*/
	if (intel_de_wait_for_clear(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
				    XELPDP_PORT_M2P_TRANSACTION_PENDING,
				    XELPDP_MSGBUS_TIMEOUT_SLOW)) {
		drm_dbg_kms(&i915->drm, "PHY %c Timeout waiting for previous transaction to complete. Reset the bus and retry.\n", phy_name(phy));
		intel_cx0_bus_reset(i915, port, lane);
		return -ETIMEDOUT;
	}

	/* Issue the write command. */
	intel_de_write(i915, XELPDP_PORT_M2P_MSGBUS_CTL(port, lane),
		       XELPDP_PORT_M2P_TRANSACTION_PENDING |
		       (committed ? XELPDP_PORT_M2P_COMMAND_WRITE_COMMITTED :
		       XELPDP_PORT_M2P_COMMAND_WRITE_UNCOMMITTED) |
		       XELPDP_PORT_M2P_DATA(data) |
		       XELPDP_PORT_M2P_ADDRESS(addr));

	/* Check for error. */
	if (committed) {
		if (intel_cx0_wait_cwrite_ack(i915, port, lane) < 0) {
			intel_cx0_bus_reset(i915, port, lane);
			return -EINVAL;
		}
	} else if ((intel_de_read(i915, XELPDP_PORT_P2M_MSGBUS_STATUS(port, lane)) &
			    XELPDP_PORT_P2M_ERROR_SET)) {
		drm_dbg_kms(&i915->drm, "PHY %c Error occurred during write command.\n", phy_name(phy));
		intel_cx0_bus_reset(i915, port, lane);
		return -EINVAL;
	}

	intel_de_write(i915, XELPDP_PORT_P2M_MSGBUS_STATUS(port, lane), ~0);

	return 0;
}

static void __intel_cx0_write(struct drm_i915_private *i915, enum port port,
			      int lane, u16 addr, u8 data, bool committed)
{
	enum phy phy = intel_port_to_phy(i915, port);
	int i, status;

	assert_dc_off(i915);

	for (i = 0; i < 3; i++) {
		status = __intel_cx0_write_once(i915, port, lane, addr, data, committed);

		if (status == 0)
			break;
	}

	if (i == 3) {
		drm_err_once(&i915->drm, "PHY %c Write %04x failed after %d retries.\n", phy_name(phy), addr, i);
		return;
	}
}

static void intel_cx0_write(struct drm_i915_private *i915, enum port port,
			    int lane, u16 addr, u8 data, bool committed)
{
	if (lane == INTEL_CX0_BOTH_LANES) {
		__intel_cx0_write(i915, port, INTEL_CX0_LANE0, addr, data, committed);
		__intel_cx0_write(i915, port, INTEL_CX0_LANE1, addr, data, committed);
	} else {
		__intel_cx0_write(i915, port, lane, addr, data, committed);
	}
}

static void intel_c20_write(struct drm_i915_private *i915, enum port port,
			    int lane, u16 addr, u16 data)
{
	assert_dc_off(i915);

	intel_cx0_write(i915, port, lane, PHY_C20_WR_ADDRESS_H, (addr >> 8) & 0xff, 0);
	intel_cx0_write(i915, port, lane, PHY_C20_WR_ADDRESS_L, addr & 0xff, 0);

	intel_cx0_write(i915, port, lane, PHY_C20_WR_DATA_H, (data >> 8) & 0xff, 0);
	intel_cx0_write(i915, port, lane, PHY_C20_WR_DATA_L, data & 0xff, 1);
}

static u16 intel_c20_read(struct drm_i915_private *i915, enum port port,
			  int lane, u16 addr)
{
	u16 val;

	assert_dc_off(i915);

	intel_cx0_write(i915, port, lane, PHY_C20_RD_ADDRESS_L, addr & 0xff, 0);
	intel_cx0_write(i915, port, lane, PHY_C20_RD_ADDRESS_H, (addr >> 8) & 0xff, 1);

	val = intel_cx0_read(i915, port, lane, PHY_C20_RD_DATA_H);
	val <<= 8;
	val |= intel_cx0_read(i915, port, lane, PHY_C20_RD_DATA_L);

	return val;
}

static void __intel_cx0_rmw(struct drm_i915_private *i915, enum port port,
			    int lane, u16 addr, u8 clear, u8 set, bool committed)
{
	u8 old, val;

	old = intel_cx0_read(i915, port, lane, addr);
	val = (old & ~clear) | set;

	if (val != old)
		intel_cx0_write(i915, port, lane, addr, val, committed);
}

static void intel_cx0_rmw(struct drm_i915_private *i915, enum port port,
			  int lane, u16 addr, u8 clear, u8 set, bool committed)
{
	if (lane == INTEL_CX0_BOTH_LANES) {
		__intel_cx0_rmw(i915, port, INTEL_CX0_LANE0, addr, clear, set, committed);
		__intel_cx0_rmw(i915, port, INTEL_CX0_LANE1, addr, clear, set, committed);
	} else {
		__intel_cx0_rmw(i915, port, lane, addr, clear, set, committed);
	}
}

/*
 * Prepare HW for CX0 phy transactions.
 *
 * It is required that PSR and DC5/6 are disabled before any CX0 message
 * bus transaction is executed.
 */
static intel_wakeref_t intel_cx0_phy_transaction_begin(struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

	intel_psr_pause(intel_dp);
	return intel_display_power_get(i915, POWER_DOMAIN_DC_OFF);
}

static void intel_cx0_phy_transaction_end(struct intel_encoder *encoder, intel_wakeref_t wakeref)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

	intel_psr_resume(intel_dp);
	intel_display_power_put(i915, POWER_DOMAIN_DC_OFF, wakeref);
}

void intel_cx0_phy_set_signal_levels(struct intel_encoder *encoder,
				     const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	bool lane_reversal = dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL;
	u8 master_lane = lane_reversal ? INTEL_CX0_LANE1 :
					 INTEL_CX0_LANE0;
	const struct intel_ddi_buf_trans *trans;
	intel_wakeref_t wakeref;
	int n_entries, ln;

	wakeref = intel_cx0_phy_transaction_begin(encoder);

	trans = encoder->get_buf_trans(encoder, crtc_state, &n_entries);
	if (drm_WARN_ON_ONCE(&i915->drm, !trans))
		return;

	intel_cx0_rmw(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C10_VDR_CONTROL(1),
		      0, C10_VDR_CTRL_MSGBUS_ACCESS, MB_WRITE_COMMITTED);

	for (ln = 0; ln < 4; ln++) {
		int level = intel_ddi_level(encoder, crtc_state, ln);
		int lane, tx;

		lane = ln / 2;
		tx = ln % 2 + 1;

		intel_cx0_rmw(i915, encoder->port, lane, PHY_CX0_VDR_OVRD_CONTROL(lane, tx, 0),
				C10_PHY_OVRD_LEVEL_MASK,
				C10_PHY_OVRD_LEVEL(trans->entries[level].snps.pre_cursor),
				MB_WRITE_COMMITTED);
		intel_cx0_rmw(i915, encoder->port, lane, PHY_CX0_VDR_OVRD_CONTROL(lane, tx, 1),
				C10_PHY_OVRD_LEVEL_MASK,
				C10_PHY_OVRD_LEVEL(trans->entries[level].snps.vswing),
				MB_WRITE_COMMITTED);
		intel_cx0_rmw(i915, encoder->port, lane, PHY_CX0_VDR_OVRD_CONTROL(lane, tx, 2),
				C10_PHY_OVRD_LEVEL_MASK,
				C10_PHY_OVRD_LEVEL(trans->entries[level].snps.post_cursor),
				MB_WRITE_COMMITTED);
	}
	/* Write Override enables in 0xD71 */
	intel_cx0_rmw(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C10_VDR_OVRD,
			PHY_C10_VDR_OVRD_TX1 | PHY_C10_VDR_OVRD_TX2,
			PHY_C10_VDR_OVRD_TX1 | PHY_C10_VDR_OVRD_TX2,
			MB_WRITE_COMMITTED);
	intel_cx0_write(i915, encoder->port, !master_lane, PHY_C10_VDR_CONTROL(1),
			C10_VDR_CTRL_MSGBUS_ACCESS | C10_VDR_CTRL_UPDATE_CFG,
			MB_WRITE_COMMITTED);
	intel_cx0_write(i915, encoder->port, master_lane, PHY_C10_VDR_CONTROL(1),
			C10_VDR_CTRL_MASTER_LANE | C10_VDR_CTRL_MSGBUS_ACCESS |
			C10_VDR_CTRL_UPDATE_CFG, MB_WRITE_COMMITTED);

	intel_cx0_phy_transaction_end(encoder, wakeref);
}

/*
 * Basic DP link rates with 38.4 MHz reference clock.
 * Note: The tables below are with SSC. In non-ssc
 * registers 0xC04 to 0xC08(pll[4] to pll[8]) will be
 * programmed 0.
 */

static const struct intel_c10mpllb_state mtl_c10_dp_rbr = {
	.clock = 162000,
	.pll[0] = 0xB4,
	.pll[1] = 0,
	.pll[2] = 0x30,
	.pll[3] = 0x1,
	.pll[4] = 0x26,
	.pll[5] = 0x0C,
	.pll[6] = 0x98,
	.pll[7] = 0x46,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xC0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x2,
	.pll[16] = 0x84,
	.pll[17] = 0x4F,
	.pll[18] = 0xE5,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_edp_r216 = {
	.clock = 216000,
	.pll[0] = 0x4,
	.pll[1] = 0,
	.pll[2] = 0xA2,
	.pll[3] = 0x1,
	.pll[4] = 0x33,
	.pll[5] = 0x10,
	.pll[6] = 0x75,
	.pll[7] = 0xB3,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x2,
	.pll[16] = 0x85,
	.pll[17] = 0x0F,
	.pll[18] = 0xE6,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_edp_r243 = {
	.clock = 243000,
	.pll[0] = 0x34,
	.pll[1] = 0,
	.pll[2] = 0xDA,
	.pll[3] = 0x1,
	.pll[4] = 0x39,
	.pll[5] = 0x12,
	.pll[6] = 0xE3,
	.pll[7] = 0xE9,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0x20,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x2,
	.pll[16] = 0x85,
	.pll[17] = 0x8F,
	.pll[18] = 0xE6,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_dp_hbr1 = {
	.clock = 270000,
	.pll[0] = 0xF4,
	.pll[1] = 0,
	.pll[2] = 0xF8,
	.pll[3] = 0x0,
	.pll[4] = 0x20,
	.pll[5] = 0x0A,
	.pll[6] = 0x29,
	.pll[7] = 0x10,
	.pll[8] = 0x1,   /* Verify */
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xA0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x1,
	.pll[16] = 0x84,
	.pll[17] = 0x4F,
	.pll[18] = 0xE5,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_edp_r324 = {
	.clock = 324000,
	.pll[0] = 0xB4,
	.pll[1] = 0,
	.pll[2] = 0x30,
	.pll[3] = 0x1,
	.pll[4] = 0x26,
	.pll[5] = 0x0C,
	.pll[6] = 0x98,
	.pll[7] = 0x46,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xC0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x1,
	.pll[16] = 0x85,
	.pll[17] = 0x4F,
	.pll[18] = 0xE6,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_edp_r432 = {
	.clock = 432000,
	.pll[0] = 0x4,
	.pll[1] = 0,
	.pll[2] = 0xA2,
	.pll[3] = 0x1,
	.pll[4] = 0x33,
	.pll[5] = 0x10,
	.pll[6] = 0x75,
	.pll[7] = 0xB3,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x1,
	.pll[16] = 0x85,
	.pll[17] = 0x0F,
	.pll[18] = 0xE6,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_dp_hbr2 = {
	.clock = 540000,
	.pll[0] = 0xF4,
	.pll[1] = 0,
	.pll[2] = 0xF8,
	.pll[3] = 0,
	.pll[4] = 0x20,
	.pll[5] = 0x0A,
	.pll[6] = 0x29,
	.pll[7] = 0x10,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xA0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0,
	.pll[16] = 0x84,
	.pll[17] = 0x4F,
	.pll[18] = 0xE5,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_edp_r675 = {
	.clock = 675000,
	.pll[0] = 0xB4,
	.pll[1] = 0,
	.pll[2] = 0x3E,
	.pll[3] = 0x1,
	.pll[4] = 0xA8,
	.pll[5] = 0x0C,
	.pll[6] = 0x33,
	.pll[7] = 0x54,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xC8,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0,
	.pll[16] = 0x85,
	.pll[17] = 0x8F,
	.pll[18] = 0xE6,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_dp_hbr3 = {
	.clock = 810000,
	.pll[0] = 0x34,
	.pll[1] = 0,
	.pll[2] = 0x84,
	.pll[3] = 0x1,
	.pll[4] = 0x30,
	.pll[5] = 0x0F,
	.pll[6] = 0x3D,
	.pll[7] = 0x98,
	.pll[8] = 0x1,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0xF0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0,
	.pll[16] = 0x84,
	.pll[17] = 0x0F,
	.pll[18] = 0xE5,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state * const mtl_c10_dp_tables[] = {
	&mtl_c10_dp_rbr,
	&mtl_c10_dp_hbr1,
	&mtl_c10_dp_hbr2,
	&mtl_c10_dp_hbr3,
	NULL,
};

static const struct intel_c10mpllb_state * const mtl_c10_edp_tables[] = {
	&mtl_c10_dp_rbr,
	&mtl_c10_edp_r216,
	&mtl_c10_edp_r243,
	&mtl_c10_dp_hbr1,
	&mtl_c10_edp_r324,
	&mtl_c10_edp_r432,
	&mtl_c10_dp_hbr2,
	&mtl_c10_edp_r675,
	&mtl_c10_dp_hbr3,
	NULL,
};

/* C20 basic DP 1.4 tables */
static const struct intel_c20pll_state mtl_c20_dp_rbr = {
	.clock = 162000,
	.tx = {	0xbe88, /* tx cfg0 */
		0x5800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mpllb = { 0x50a8,	/* mpllb cfg0 */
		0x2120,		/* mpllb cfg1 */
		0xcd9a,		/* mpllb cfg2 */
		0xbfc1,		/* mpllb cfg3 */
		0x5ab8,         /* mpllb cfg4 */
		0x4c34,         /* mpllb cfg5 */
		0x2000,		/* mpllb cfg6 */
		0x0001,		/* mpllb cfg7 */
		0x6000,		/* mpllb cfg8 */
		0x0000,		/* mpllb cfg9 */
		0x0000,		/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_dp_hbr1 = {
	.clock = 270000,
	.tx = {	0xbe88, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mpllb = { 0x308c,	/* mpllb cfg0 */
		0x2110,		/* mpllb cfg1 */
		0xcc9c,		/* mpllb cfg2 */
		0xbfc1,		/* mpllb cfg3 */
		0x489a,         /* mpllb cfg4 */
		0x3f81,         /* mpllb cfg5 */
		0x2000,		/* mpllb cfg6 */
		0x0001,		/* mpllb cfg7 */
		0x5000,		/* mpllb cfg8 */
		0x0000,		/* mpllb cfg9 */
		0x0000,		/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_dp_hbr2 = {
	.clock = 540000,
	.tx = {	0xbe88, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mpllb = { 0x108c,	/* mpllb cfg0 */
		0x2108,		/* mpllb cfg1 */
		0xcc9c,		/* mpllb cfg2 */
		0xbfc1,		/* mpllb cfg3 */
		0x489a,         /* mpllb cfg4 */
		0x3f81,         /* mpllb cfg5 */
		0x2000,		/* mpllb cfg6 */
		0x0001,		/* mpllb cfg7 */
		0x5000,		/* mpllb cfg8 */
		0x0000,		/* mpllb cfg9 */
		0x0000,		/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_dp_hbr3 = {
	.clock = 810000,
	.tx = {	0xbe88, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mpllb = { 0x10d2,	/* mpllb cfg0 */
		0x2108,		/* mpllb cfg1 */
		0x8d98,		/* mpllb cfg2 */
		0xbfc1,		/* mpllb cfg3 */
		0x7166,         /* mpllb cfg4 */
		0x5f42,         /* mpllb cfg5 */
		0x2000,		/* mpllb cfg6 */
		0x0001,		/* mpllb cfg7 */
		0x7800,		/* mpllb cfg8 */
		0x0000,		/* mpllb cfg9 */
		0x0000,		/* mpllb cfg10 */
		},
};

/* C20 basic DP 2.0 tables */
static const struct intel_c20pll_state mtl_c20_dp_uhbr10 = {
	.clock = 312500,
	.tx = {	0xbe21, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mplla = { 0x3104,	/* mplla cfg0 */
		0xd105,		/* mplla cfg1 */
		0xc025,		/* mplla cfg2 */
		0xc025,		/* mplla cfg3 */
		0xa6ab,		/* mplla cfg4 */
		0x8c00,		/* mplla cfg5 */
		0x4000,		/* mplla cfg6 */
		0x0003,		/* mplla cfg7 */
		0x3555,		/* mplla cfg8 */
		0x0001,		/* mplla cfg9 */
		},
};

static const struct intel_c20pll_state mtl_c20_dp_uhbr13_5 = {
	.clock = 421875,
	.tx = {	0xbea0, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mpllb = { 0x015f,	/* mpllb cfg0 */
		0x2205,		/* mpllb cfg1 */
		0x1b17,		/* mpllb cfg2 */
		0xffc1,		/* mpllb cfg3 */
		0xe100,		/* mpllb cfg4 */
		0xbd00,		/* mpllb cfg5 */
		0x2000,		/* mpllb cfg6 */
		0x0001,		/* mpllb cfg7 */
		0x4800,		/* mpllb cfg8 */
		0x0000,		/* mpllb cfg9 */
		0x0000,		/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_dp_uhbr20 = {
	.clock = 625000,
	.tx = {	0xbe20, /* tx cfg0 */
		0x4800, /* tx cfg1 */
		0x0000, /* tx cfg2 */
		},
	.cmn = {0x0500, /* cmn cfg0*/
		0x0005, /* cmn cfg1 */
		0x0000, /* cmn cfg2 */
		0x0000, /* cmn cfg3 */
		},
	.mplla = { 0x3104,	/* mplla cfg0 */
		0xd105,		/* mplla cfg1 */
		0xc025,		/* mplla cfg2 */
		0xc025,		/* mplla cfg3 */
		0xa6ab,		/* mplla cfg4 */
		0x8c00,		/* mplla cfg5 */
		0x4000,		/* mplla cfg6 */
		0x0003,		/* mplla cfg7 */
		0x3555,		/* mplla cfg8 */
		0x0001,		/* mplla cfg9 */
		},
};

static const struct intel_c20pll_state * const mtl_c20_dp_tables[] = {
	&mtl_c20_dp_rbr,
	&mtl_c20_dp_hbr1,
	&mtl_c20_dp_hbr2,
	&mtl_c20_dp_hbr3,
	&mtl_c20_dp_uhbr10,
	&mtl_c20_dp_uhbr13_5,
	&mtl_c20_dp_uhbr20,
	NULL,
};

/*
 * HDMI link rates with 38.4 MHz reference clock.
 */

static const struct intel_c10mpllb_state __maybe_unused mtl_c10_hdmi_25_2 = {
	.clock = 25200,
	.pll[0] = 0x4,
	.pll[1] = 0,
	.pll[2] = 0xB2,
	.pll[3] = 0,
	.pll[4] = 0,
	.pll[5] = 0,
	.pll[6] = 0,
	.pll[7] = 0,
	.pll[8] = 0x20,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0xD,
	.pll[16] = 0x6,
	.pll[17] = 0x8F,
	.pll[18] = 0x84,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_27_0 = {
	.clock = 27000,
	.pll[0] = 0x34,
	.pll[1] = 0,
	.pll[2] = 0xC0,
	.pll[3] = 0,
	.pll[4] = 0,
	.pll[5] = 0,
	.pll[6] = 0,
	.pll[7] = 0,
	.pll[8] = 0x20,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0x80,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0xD,
	.pll[16] = 0x6,
	.pll[17] = 0xCF,
	.pll[18] = 0x84,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_74_25 = {
	.clock = 74250,
	.pll[0] = 0xF4,
	.pll[1] = 0,
	.pll[2] = 0x7A,
	.pll[3] = 0,
	.pll[4] = 0,
	.pll[5] = 0,
	.pll[6] = 0,
	.pll[7] = 0,
	.pll[8] = 0x20,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0x58,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0xB,
	.pll[16] = 0x6,
	.pll[17] = 0xF,
	.pll[18] = 0x85,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_148_5 = {
	.clock = 148500,
	.pll[0] = 0xF4,
	.pll[1] = 0,
	.pll[2] = 0x7A,
	.pll[3] = 0,
	.pll[4] = 0,
	.pll[5] = 0,
	.pll[6] = 0,
	.pll[7] = 0,
	.pll[8] = 0x20,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0x58,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0xA,
	.pll[16] = 0x6,
	.pll[17] = 0xF,
	.pll[18] = 0x85,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_594 = {
	.clock = 594000,
	.pll[0] = 0xF4,
	.pll[1] = 0,
	.pll[2] = 0x7A,
	.pll[3] = 0,
	.pll[4] = 0,
	.pll[5] = 0,
	.pll[6] = 0,
	.pll[7] = 0,
	.pll[8] = 0x20,
	.pll[9] = 0x1,
	.pll[10] = 0,
	.pll[11] = 0,
	.pll[12] = 0x58,
	.pll[13] = 0,
	.pll[14] = 0,
	.pll[15] = 0x8,
	.pll[16] = 0x6,
	.pll[17] = 0xF,
	.pll[18] = 0x85,
	.pll[19] = 0x23,
};

/* Precomputed C10 HDMI PLL tables */
static const struct intel_c10mpllb_state mtl_c10_hdmi_25175 = {
	.clock = 25175,
	.pll[0] = 0x34,
	.pll[1] = 0x00,
	.pll[2] = 0xB0,
	.pll[3] = 0x00,
	.pll[4] = 0x00,
	.pll[5] = 0x00,
	.pll[6] = 0x00,
	.pll[7] = 0x00,
	.pll[8] = 0x20,
	.pll[9] = 0xFF,
	.pll[10] = 0xFF,
	.pll[11] = 0x55,
	.pll[12] = 0xE5,
	.pll[13] = 0x55,
	.pll[14] = 0x55,
	.pll[15] = 0x0D,
	.pll[16] = 0x09,
	.pll[17] = 0x8F,
	.pll[18] = 0x84,
	.pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_25200 = {
	.clock = 25200,
	.pll[0] = 0x04, .pll[1] = 0x00, .pll[2] = 0xB2, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x00, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0D, .pll[16] = 0x09, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_27027 = {
	.clock = 27027,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xC0, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xCC, .pll[12] = 0x9C, .pll[13] = 0xCB, .pll[14] = 0xCC,
	.pll[15] = 0x0D, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_28320 = {
	.clock = 28320,
	.pll[0] = 0x04, .pll[1] = 0x00, .pll[2] = 0xCC, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x00, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0D, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_30240 = {
	.clock = 30240,
	.pll[0] = 0x04, .pll[1] = 0x00, .pll[2] = 0xDC, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x00, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0D, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_31500 = {
	.clock = 31500,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x62, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0xA0, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0C, .pll[16] = 0x09, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_36000 = {
	.clock = 36000,
	.pll[0] = 0xC4, .pll[1] = 0x00, .pll[2] = 0x76, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x00, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_40000 = {
	.clock = 40000,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x86, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0x55, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_49500 = {
	.clock = 49500,
	.pll[0] = 0x74, .pll[1] = 0x00, .pll[2] = 0xAE, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x20, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_50000 = {
	.clock = 50000,
	.pll[0] = 0x74, .pll[1] = 0x00, .pll[2] = 0xB0, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0x2A, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_57284 = {
	.clock = 57284,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xCE, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x77, .pll[12] = 0x57, .pll[13] = 0x77, .pll[14] = 0x77,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_58000 = {
	.clock = 58000,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xD0, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0xD5, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0C, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_65000 = {
	.clock = 65000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x66, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0xB5, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0B, .pll[16] = 0x09, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_71000 = {
	.clock = 71000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x72, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0xF5, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_74176 = {
	.clock = 74176,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x44, .pll[12] = 0x44, .pll[13] = 0x44, .pll[14] = 0x44,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_75000 = {
	.clock = 75000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7C, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x20, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_78750 = {
	.clock = 78750,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x84, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x08, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_85500 = {
	.clock = 85500,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x92, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x10, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_88750 = {
	.clock = 88750,
	.pll[0] = 0x74, .pll[1] = 0x00, .pll[2] = 0x98, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0x72, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x0B, .pll[16] = 0x09, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_106500 = {
	.clock = 106500,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xBC, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0xF0, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_108000 = {
	.clock = 108000,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xC0, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x80, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_115500 = {
	.clock = 115500,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xD0, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x50, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_119000 = {
	.clock = 119000,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xD6, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0xF5, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0B, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_135000 = {
	.clock = 135000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x6C, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x50, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0A, .pll[16] = 0x09, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_138500 = {
	.clock = 138500,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x70, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0x22, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_147160 = {
	.clock = 147160,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x78, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0xA5, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_148352 = {
	.clock = 148352,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x44, .pll[12] = 0x44, .pll[13] = 0x44, .pll[14] = 0x44,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_154000 = {
	.clock = 154000,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x80, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0x35, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_162000 = {
	.clock = 162000,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x88, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x60, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_167000 = {
	.clock = 167000,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x8C, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0xFA, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_197802 = {
	.clock = 197802,
	.pll[0] = 0x74, .pll[1] = 0x00, .pll[2] = 0xAE, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x99, .pll[12] = 0x05, .pll[13] = 0x98, .pll[14] = 0x99,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_198000 = {
	.clock = 198000,
	.pll[0] = 0x74, .pll[1] = 0x00, .pll[2] = 0xAE, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x20, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_209800 = {
	.clock = 209800,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xBA, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0x45, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_241500 = {
	.clock = 241500,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xDA, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0xC8, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x0A, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_262750 = {
	.clock = 262750,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x68, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0x6C, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x09, .pll[16] = 0x09, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_268500 = {
	.clock = 268500,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x6A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0xEC, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x09, .pll[16] = 0x09, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_296703 = {
	.clock = 296703,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x33, .pll[12] = 0x44, .pll[13] = 0x33, .pll[14] = 0x33,
	.pll[15] = 0x09, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_297000 = {
	.clock = 297000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x00, .pll[12] = 0x58, .pll[13] = 0x00, .pll[14] = 0x00,
	.pll[15] = 0x09, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_319750 = {
	.clock = 319750,
	.pll[0] = 0xB4, .pll[1] = 0x00, .pll[2] = 0x86, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0xAA, .pll[12] = 0x44, .pll[13] = 0xA9, .pll[14] = 0xAA,
	.pll[15] = 0x09, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_497750 = {
	.clock = 497750,
	.pll[0] = 0x34, .pll[1] = 0x00, .pll[2] = 0xE2, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0x9F, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x09, .pll[16] = 0x08, .pll[17] = 0xCF, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_592000 = {
	.clock = 592000,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x55, .pll[12] = 0x15, .pll[13] = 0x55, .pll[14] = 0x55,
	.pll[15] = 0x08, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state mtl_c10_hdmi_593407 = {
	.clock = 593407,
	.pll[0] = 0xF4, .pll[1] = 0x00, .pll[2] = 0x7A, .pll[3] = 0x00, .pll[4] = 0x00,
	.pll[5] = 0x00, .pll[6] = 0x00, .pll[7] = 0x00, .pll[8] = 0x20, .pll[9] = 0xFF,
	.pll[10] = 0xFF, .pll[11] = 0x3B, .pll[12] = 0x44, .pll[13] = 0xBA, .pll[14] = 0xBB,
	.pll[15] = 0x08, .pll[16] = 0x08, .pll[17] = 0x8F, .pll[18] = 0x84, .pll[19] = 0x23,
};

static const struct intel_c10mpllb_state * const mtl_c10_hdmi_tables[] = {
	&mtl_c10_hdmi_25175,
	&mtl_c10_hdmi_25200, /* Consolidated Table */
	&mtl_c10_hdmi_27_0, /* Consolidated Table */
	&mtl_c10_hdmi_27027,
	&mtl_c10_hdmi_28320,
	&mtl_c10_hdmi_30240,
	&mtl_c10_hdmi_31500,
	&mtl_c10_hdmi_36000,
	&mtl_c10_hdmi_40000,
	&mtl_c10_hdmi_49500,
	&mtl_c10_hdmi_50000,
	&mtl_c10_hdmi_57284,
	&mtl_c10_hdmi_58000,
	&mtl_c10_hdmi_65000,
	&mtl_c10_hdmi_71000,
	&mtl_c10_hdmi_74176,
	&mtl_c10_hdmi_74_25, /* Consolidated Table */
	&mtl_c10_hdmi_75000,
	&mtl_c10_hdmi_78750,
	&mtl_c10_hdmi_85500,
	&mtl_c10_hdmi_88750,
	&mtl_c10_hdmi_106500,
	&mtl_c10_hdmi_108000,
	&mtl_c10_hdmi_115500,
	&mtl_c10_hdmi_119000,
	&mtl_c10_hdmi_135000,
	&mtl_c10_hdmi_138500,
	&mtl_c10_hdmi_147160,
	&mtl_c10_hdmi_148352,
	&mtl_c10_hdmi_148_5, /* Consolidated Table */
	&mtl_c10_hdmi_154000,
	&mtl_c10_hdmi_162000,
	&mtl_c10_hdmi_167000,
	&mtl_c10_hdmi_197802,
	&mtl_c10_hdmi_198000,
	&mtl_c10_hdmi_209800,
	&mtl_c10_hdmi_241500,
	&mtl_c10_hdmi_262750,
	&mtl_c10_hdmi_268500,
	&mtl_c10_hdmi_296703,
	&mtl_c10_hdmi_297000,
	&mtl_c10_hdmi_319750,
	&mtl_c10_hdmi_497750,
	&mtl_c10_hdmi_592000,
	&mtl_c10_hdmi_593407,
	&mtl_c10_hdmi_594, /* Consolidated Table */
	NULL,
};

static const struct intel_c20pll_state mtl_c20_hdmi_25_175 = {
	.clock = 25175,
	.mpllb = { 0xa0d2,	/* mpllb cfg0 */
		   0x7d80,	/* mpllb cfg1 */
		   0x0906,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x0200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x0000,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0001,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_27_0 = {
	.clock = 27000,
	.mpllb = { 0xa0e0,	/* mpllb cfg0 */
		   0x7d80,	/* mpllb cfg1 */
		   0x0906,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x8000,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0001,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_74_25 = {
	.clock = 74250,
	.mpllb = { 0x609a,	/* mpllb cfg0 */
		   0x7d40,	/* mpllb cfg1 */
		   0xca06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x5800,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0001,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_148_5 = {
	.clock = 148500,
	.mpllb = { 0x409a,	/* mpllb cfg0 */
		   0x7d20,	/* mpllb cfg1 */
		   0xca06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x5800,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0001,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_594 = {
	.clock = 594000,
	.mpllb = { 0x009a,	/* mpllb cfg0 */
		   0x7d08,	/* mpllb cfg1 */
		   0xca06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x5800,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0001,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_300 = {
	.clock = 166670,
	.mpllb = { 0x209c,	/* mpllb cfg0 */
		   0x7d10,	/* mpllb cfg1 */
		   0xca06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x2000,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0004,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_600 = {
	.clock = 333330,
	.mpllb = { 0x009c,	/* mpllb cfg0 */
		   0x7d08,	/* mpllb cfg1 */
		   0xca06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x2000,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0004,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_800 = {
	.clock = 444440,
	.mpllb = { 0x00d0,	/* mpllb cfg0 */
		   0x7d08,	/* mpllb cfg1 */
		   0x4a06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0003,	/* mpllb cfg7 */
		   0x2aaa,	/* mpllb cfg8 */
		   0x0002,	/* mpllb cfg9 */
		   0x0004,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_1000 = {
	.clock = 555560,
	.mpllb = { 0x1104,	/* mpllb cfg0 */
		   0x7d08,	/* mpllb cfg1 */
		   0x0a06,	/* mpllb cfg2 */
		   0xbe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0003,	/* mpllb cfg7 */
		   0x3555,	/* mpllb cfg8 */
		   0x0001,	/* mpllb cfg9 */
		   0x0004,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state mtl_c20_hdmi_1200 = {
	.clock = 666670,
	.mpllb = { 0x0138,	/* mpllb cfg0 */
		   0x7d08,	/* mpllb cfg1 */
		   0x5486,	/* mpllb cfg2 */
		   0xfe40,	/* mpllb cfg3 */
		   0x0000,	/* mpllb cfg4 */
		   0x0000,	/* mpllb cfg5 */
		   0x2200,	/* mpllb cfg6 */
		   0x0001,	/* mpllb cfg7 */
		   0x4000,	/* mpllb cfg8 */
		   0x0000,	/* mpllb cfg9 */
		   0x0004,	/* mpllb cfg10 */
		},
};

static const struct intel_c20pll_state * const mtl_c20_hdmi_tables[] = {
	&mtl_c20_hdmi_25_175,
	&mtl_c20_hdmi_27_0,
	&mtl_c20_hdmi_74_25,
	&mtl_c20_hdmi_148_5,
	&mtl_c20_hdmi_594,
	&mtl_c20_hdmi_300,
	&mtl_c20_hdmi_600,
	&mtl_c20_hdmi_800,
	&mtl_c20_hdmi_1000,
	&mtl_c20_hdmi_1200,
	NULL,
};

static int intel_c10_phy_check_hdmi_link_rate(int clock)
{
	const struct intel_c10mpllb_state * const *tables = mtl_c10_hdmi_tables;
	int i;

	for (i = 0; tables[i]; i++) {
		if (clock == tables[i]->clock)
			return MODE_OK;
	}

	return MODE_CLOCK_RANGE;
}

int intel_cx0_phy_check_hdmi_link_rate(struct intel_hdmi *hdmi, int clock)
{
	struct intel_digital_port *dig_port = hdmi_to_dig_port(hdmi);
	struct drm_i915_private *i915 = intel_hdmi_to_i915(hdmi);
	enum phy phy = intel_port_to_phy(i915, dig_port->base.port);

	if (intel_is_c10phy(i915, phy))
		return intel_c10_phy_check_hdmi_link_rate(clock);
	return intel_c20_phy_check_hdmi_link_rate(clock);
}

static const struct intel_c10mpllb_state * const *
intel_c10_mpllb_tables_get(struct intel_crtc_state *crtc_state,
			   struct intel_encoder *encoder)
{
	if (intel_crtc_has_dp_encoder(crtc_state)) {
		if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
			return mtl_c10_edp_tables;
		else
			return mtl_c10_dp_tables;
	} else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		return mtl_c10_hdmi_tables;
	}

	MISSING_CASE(encoder->type);
	return NULL;
}

static int intel_c10mpllb_calc_state(struct intel_crtc_state *crtc_state,
				     struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	const struct intel_c10mpllb_state * const *tables;
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	int i;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		if (intel_c10_phy_check_hdmi_link_rate(crtc_state->port_clock)
		    != MODE_OK) {
			drm_dbg_kms(&i915->drm, "Can't support HDMI link rate %d on phy %c.\n",
				      crtc_state->port_clock, phy_name(phy));
			return -EINVAL;
		}
	}

	tables = intel_c10_mpllb_tables_get(crtc_state, encoder);
	if (!tables)
		return -EINVAL;

	for (i = 0; tables[i]; i++) {
		if (crtc_state->port_clock <= tables[i]->clock) {
			crtc_state->cx0pll_state.c10mpllb_state = *tables[i];
			return 0;
		}
	}

	return -EINVAL;
}

int intel_cx0mpllb_calc_state(struct intel_crtc_state *crtc_state,
			      struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);

	if (intel_is_c10phy(i915, phy))
		return intel_c10mpllb_calc_state(crtc_state, encoder);
	else
		return intel_c20pll_calc_state(crtc_state, encoder);
}

void intel_c10mpllb_readout_hw_state(struct intel_encoder *encoder,
				     struct intel_c10mpllb_state *pll_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	bool lane_reversal = dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL;
	u8 lane = lane_reversal ? INTEL_CX0_LANE1 :
				  INTEL_CX0_LANE0;
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	intel_wakeref_t wakeref;
	int i;
	u8 cmn, tx0;

	wakeref = intel_cx0_phy_transaction_begin(encoder);

	/*
	 * According to C10 VDR Register programming Sequence we need
	 * to do this to read PHY internal registers from MsgBus.
	 */
	intel_cx0_rmw(i915, encoder->port, lane, PHY_C10_VDR_CONTROL(1), 0,
		      C10_VDR_CTRL_MSGBUS_ACCESS, MB_WRITE_COMMITTED);

	for (i = 0; i < ARRAY_SIZE(pll_state->pll); i++)
		pll_state->pll[i] = intel_cx0_read(i915, encoder->port, lane,
						   PHY_C10_VDR_PLL(i));

	cmn = intel_cx0_read(i915, encoder->port, lane, PHY_C10_VDR_CMN(0));
	tx0 = intel_cx0_read(i915, encoder->port, lane, PHY_C10_VDR_TX(0));

	if (tx0 != C10_TX0_VAL || (cmn != C10_CMN0_DP_VAL &&
				   cmn != C10_CMN0_HDMI_VAL))
		drm_dbg_kms(&i915->drm, "Unexpected tx: %x or cmn: %x for phy: %c.\n",
			    tx0, cmn, phy_name(phy));

	intel_cx0_phy_transaction_end(encoder, wakeref);
}

static void intel_c10_pll_program(struct drm_i915_private *i915,
				  const struct intel_crtc_state *crtc_state,
				  struct intel_encoder *encoder)
{
	const struct intel_c10mpllb_state *pll_state = &crtc_state->cx0pll_state.c10mpllb_state;
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	bool lane_reversal = dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL;
	u8 master_lane = lane_reversal ? INTEL_CX0_LANE1 :
					 INTEL_CX0_LANE0;
	u8 follower_lane = lane_reversal ? INTEL_CX0_LANE0 :
					   INTEL_CX0_LANE1;
	int i;
	struct intel_dp *intel_dp;
	bool use_ssc = false;
	bool use_hdmi = false;
	u8 cmn0;

	if (intel_crtc_has_dp_encoder(crtc_state)) {
		intel_dp = enc_to_intel_dp(encoder);
		use_ssc = (intel_dp->dpcd[DP_MAX_DOWNSPREAD] &
			  DP_MAX_DOWNSPREAD_0_5);

		if (!intel_panel_use_ssc(i915))
			use_ssc = false;

		cmn0 = C10_CMN0_DP_VAL;
	} else {
		use_hdmi = true;
		cmn0 = C10_CMN0_HDMI_VAL;
	}

	intel_cx0_write(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C10_VDR_CONTROL(1),
			C10_VDR_CTRL_MSGBUS_ACCESS, MB_WRITE_COMMITTED);
	/* Custom width needs to be programmed to 0 for both the phy lanes */
	intel_cx0_rmw(i915, encoder->port, INTEL_CX0_BOTH_LANES,
		      PHY_C10_VDR_CUSTOM_WIDTH, 0x3, 0, MB_WRITE_COMMITTED);
	intel_cx0_rmw(i915, encoder->port, follower_lane, PHY_C10_VDR_CONTROL(1),
		      C10_VDR_CTRL_MASTER_LANE, C10_VDR_CTRL_UPDATE_CFG,
		      MB_WRITE_COMMITTED);

	/* Program the pll values only for the master lane */
	for (i = 0; i < ARRAY_SIZE(pll_state->pll); i++)
		/* If not using ssc pll[4] through pll[8] must be 0*/
		intel_cx0_write(i915, encoder->port, master_lane, PHY_C10_VDR_PLL(i),
				(!(use_ssc || use_hdmi) && (i > 3 && i < 9)) ? 0 : pll_state->pll[i],
				(i % 4) ? MB_WRITE_UNCOMMITTED : MB_WRITE_COMMITTED);

	intel_cx0_write(i915, encoder->port, master_lane, PHY_C10_VDR_CMN(0), cmn0, MB_WRITE_COMMITTED);
	intel_cx0_write(i915, encoder->port, master_lane, PHY_C10_VDR_TX(0), C10_TX0_VAL, MB_WRITE_COMMITTED);
	intel_cx0_rmw(i915, encoder->port, master_lane, PHY_C10_VDR_CONTROL(1),
		      C10_VDR_CTRL_MSGBUS_ACCESS, C10_VDR_CTRL_MASTER_LANE |
		      C10_VDR_CTRL_UPDATE_CFG, MB_WRITE_COMMITTED);
}

void intel_c10mpllb_dump_hw_state(struct drm_i915_private *dev_priv,
				  const struct intel_c10mpllb_state *hw_state)
{
	bool fracen;
	int i;
	unsigned int frac_quot = 0, frac_rem = 0, frac_den = 1;
	unsigned int multiplier, tx_clk_div;

	fracen = hw_state->pll[0] & C10_PLL0_FRACEN;
	drm_dbg_kms(&dev_priv->drm, "c10pll_hw_state: fracen: %s, ",
		    str_yes_no(fracen));

	if (fracen) {
		frac_quot = hw_state->pll[12] << 8 | hw_state->pll[11];
		frac_rem =  hw_state->pll[14] << 8 | hw_state->pll[13];
		frac_den =  hw_state->pll[10] << 8 | hw_state->pll[9];
		drm_dbg_kms(&dev_priv->drm, "quot: %u, rem: %u, den: %u,\n",
			    frac_quot, frac_rem, frac_den);
	}

	multiplier = (REG_FIELD_GET8(C10_PLL3_MULTIPLIERH_MASK, hw_state->pll[3]) << 8 |
		      hw_state->pll[2]) / 2 + 16;
	tx_clk_div = REG_FIELD_GET8(C10_PLL15_TXCLKDIV_MASK, hw_state->pll[15]);
	drm_dbg_kms(&dev_priv->drm,
		    "multiplier: %u, tx_clk_div: %u.\n", multiplier, tx_clk_div);

	drm_dbg_kms(&dev_priv->drm, "c10pll_rawhw_state:");

	for (i = 0; i < ARRAY_SIZE(hw_state->pll); i = i + 4)
		drm_dbg_kms(&dev_priv->drm, "pll[%d] = 0x%x, pll[%d] = 0x%x, pll[%d] = 0x%x, pll[%d] = 0x%x\n",
			    i, hw_state->pll[i], i + 1, hw_state->pll[i + 1],
			    i + 2, hw_state->pll[i + 2], i + 3, hw_state->pll[i + 3]);
}

int intel_c20_phy_check_hdmi_link_rate(int clock)
{
	const struct intel_c20pll_state * const *tables = mtl_c20_hdmi_tables;
	int i;

	for (i = 0; tables[i]; i++) {
		if (clock == tables[i]->clock)
			return MODE_OK;
	}

	return MODE_CLOCK_RANGE;
}

static const struct intel_c20pll_state * const *
intel_c20_pll_tables_get(struct intel_crtc_state *crtc_state,
			 struct intel_encoder *encoder)
{
	if (intel_crtc_has_dp_encoder(crtc_state)) {
		return mtl_c20_dp_tables;
	} else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		return mtl_c20_hdmi_tables;
	}

	MISSING_CASE(encoder->type);
	return NULL;
}

int intel_c20pll_calc_state(struct intel_crtc_state *crtc_state,
			    struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	const struct intel_c20pll_state * const *tables;
	int i;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI)) {
		if (intel_c20_phy_check_hdmi_link_rate(crtc_state->port_clock) != MODE_OK) {
			drm_dbg_kms(&i915->drm, "Can't support HDMI link rate %d on phy %c.\n",
				    crtc_state->port_clock, phy_name(phy));
			return -EINVAL;
		}
	}

	tables = intel_c20_pll_tables_get(crtc_state, encoder);
	if (!tables)
		return -EINVAL;

	for (i = 0; tables[i]; i++) {
		if (crtc_state->port_clock <= tables[i]->clock) {
			crtc_state->cx0pll_state.c20pll_state = *tables[i];
			return 0;
		}
	}

	return -EINVAL;
}

static bool intel_c20_use_mplla(u32 clock)
{
	/* 10G and 20G rates use MPLLA */
	if (clock == 312500 || clock == 625000)
		return true;

	return false;
}

void intel_c20pll_readout_hw_state(struct intel_encoder *encoder,
				   struct intel_c20pll_state *pll_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	bool cntx, use_mplla;
	u32 val;
	int i;

	/* 1. Read current context selection */
	cntx = intel_cx0_read(i915, encoder->port, 0, PHY_C20_VDR_CUSTOM_SERDES_RATE) & PHY_C20_CONTEXT_TOGGLE;

	/* Read Tx configuration */
	for (i = 0; i < ARRAY_SIZE(pll_state->tx); i++) {
		if (cntx)
			pll_state->tx[i] = intel_c20_read(i915, encoder->port, 0,
							  PHY_C20_B_TX_CNTX_CFG(i));
		else
			pll_state->tx[i] = intel_c20_read(i915, encoder->port, 0,
							  PHY_C20_A_TX_CNTX_CFG(i));
	}

	/* Read common configuration */
	for (i = 0; i < ARRAY_SIZE(pll_state->cmn); i++) {
		if (cntx)
			pll_state->cmn[i] = intel_c20_read(i915, encoder->port, 0,
							   PHY_C20_B_CMN_CNTX_CFG(i));
		else
			pll_state->cmn[i] = intel_c20_read(i915, encoder->port, 0,
							   PHY_C20_A_CMN_CNTX_CFG(i));
	}

	val = intel_c20_read(i915, encoder->port, 0, PHY_C20_A_MPLLA_CNTX_CFG(6));
	use_mplla = val & C20_MPLLB_FRACEN;

	if (use_mplla) {
		/* MPLLA configuration */
		for (i = 0; i < ARRAY_SIZE(pll_state->mplla); i++) {
			if (cntx)
				pll_state->mplla[i] = intel_c20_read(i915, encoder->port, 0,
								     PHY_C20_B_MPLLA_CNTX_CFG(i));
			else
				pll_state->mplla[i] = intel_c20_read(i915, encoder->port, 0,
								     PHY_C20_A_MPLLA_CNTX_CFG(i));
		}
	} else {
		/* MPLLB configuration */
		for (i = 0; i < ARRAY_SIZE(pll_state->mpllb); i++) {
			if (cntx)
				pll_state->mpllb[i] = intel_c20_read(i915, encoder->port, 0,
								     PHY_C20_B_MPLLB_CNTX_CFG(i));
			else
				pll_state->mpllb[i] = intel_c20_read(i915, encoder->port, 0,
								     PHY_C20_A_MPLLB_CNTX_CFG(i));
		}
	}
}

static u8 intel_c20_get_dp_rate(u32 clock)
{
	switch (clock) {
	case 162000: /* 1.62 Gbps DP1.4 */
		return 0;
	case 270000: /* 2.7 Gbps DP1.4 */
		return 1;
	case 540000: /* 5.4 Gbps DP 1.4 */
		return 2;
	case 810000: /* 8.1 Gbps DP1.4 */
		return 3;
	case 216000: /* 2.16 Gbps eDP */
		return 4;
	case 243000: /* 2.43 Gbps eDP */
		return 5;
	case 324000: /* 3.24 Gbps eDP */
		return 6;
	case 432000: /* 4.32 Gbps eDP */
		return 7;
	case 312500: /* 10 Gbps DP2.0 */
		return 8;
	case 421875: /* 13.5 Gbps DP2.0 */
		return 9;
	case 625000: /* 20 Gbps DP2.0*/
		return 10;
	default:
		MISSING_CASE(clock);
		return 0;
	}
}

static u8 intel_c20_get_hdmi_rate(u32 clock)
{
	switch (clock) {
	case 25175:
	case 27000:
	case 74250:
	case 148500:
	case 594000:
		return 0;
	case 166670: /* 3 Gbps */
	case 333330: /* 6 Gbps */
	case 666670: /* 12 Gbps */
		return 1;
	case 444440: /* 8 Gbps */
		return 2;
	case 555560: /* 10 Gbps */
		return 3;
	default:
		MISSING_CASE(clock);
		return 0;
	}
}

static bool is_dp2(u32 clock)
{
	/* DP2.0 clock rates */
	if (clock == 312500 || clock == 421875 || clock  == 625000)
		return true;

	return false;
}

static bool is_hdmi_frl(u32 clock)
{
	switch (clock) {
	case 166670: /* 3 Gbps */
	case 333330: /* 6 Gbps */
	case 444440: /* 8 Gbps */
	case 555560: /* 10 Gbps */
	case 666670: /* 12 Gbps */
		return true;
	default:
		return false;
	}
}

static bool intel_c20_protocol_switch_valid(struct intel_encoder *encoder)
{
	struct intel_digital_port *intel_dig_port = enc_to_dig_port(encoder);

	/* banks should not be cleared for DPALT/USB4/TBT modes */
	/* TODO: optimize re-calibration in legacy mode */
	return intel_tc_port_in_legacy_mode(intel_dig_port);
}

static void intel_c20_pll_program(struct drm_i915_private *i915,
				  const struct intel_crtc_state *crtc_state,
				  struct intel_encoder *encoder)
{
	const struct intel_c20pll_state *pll_state = &crtc_state->cx0pll_state.c20pll_state;
	bool dp = false;
	int lane_count = crtc_state->lane_count;
	bool cntx;
	int i;

	if (intel_crtc_has_dp_encoder(crtc_state))
		dp = true;

	/* 1. Read current context selection */
	cntx = intel_cx0_read(i915, encoder->port, 0, PHY_C20_VDR_CUSTOM_SERDES_RATE) &
		PHY_C20_CONTEXT_TOGGLE;

	/* 2. If there is a protocol switch from HDMI to DP or vice versa, clear
	 * the lane #0 MPLLB CAL_DONE_BANK DP2.0 10G and 20G rates enable MPLLA.
	 * Protocol switch is only applicable for MPLLA
	 */
	if (intel_c20_protocol_switch_valid(encoder)) {
		for (i = 0; i < 4; i++)
			intel_c20_write(i915, encoder->port, 0, RAWLANEAONX_DIG_TX_MPLLB_CAL_DONE_BANK(i), 0);
	}

	/* 3. Write SRAM configuration context. If A in use, write configuration to B context */
	/* 3.1 Tx configuration */
	for (i = 0; i < 3; i++) {
		if (cntx)
			intel_c20_write(i915, encoder->port, 0, PHY_C20_A_TX_CNTX_CFG(i), pll_state->tx[i]);
		else
			intel_c20_write(i915, encoder->port, 0, PHY_C20_B_TX_CNTX_CFG(i), pll_state->tx[i]);
	}

	/* 3.2 common configuration */
	for (i = 0; i < 4; i++) {
		if (cntx)
			intel_c20_write(i915, encoder->port, 0, PHY_C20_A_CMN_CNTX_CFG(i), pll_state->cmn[i]);
		else
			intel_c20_write(i915, encoder->port, 0, PHY_C20_B_CMN_CNTX_CFG(i), pll_state->cmn[i]);
	}

	/* 3.3 mpllb or mplla configuration */
	if (intel_c20_use_mplla(pll_state->clock)) {
		for (i = 0; i < 10; i++) {
			if (cntx)
				intel_c20_write(i915, encoder->port, 0,
						PHY_C20_A_MPLLA_CNTX_CFG(i),
						pll_state->mplla[i]);
			else
				intel_c20_write(i915, encoder->port, 0,
						PHY_C20_B_MPLLA_CNTX_CFG(i),
						pll_state->mplla[i]);
		}
	} else {
		for (i = 0; i < 11; i++) {
			if (cntx)
				intel_c20_write(i915, encoder->port, 0,
						PHY_C20_A_MPLLB_CNTX_CFG(i),
						pll_state->mpllb[i]);
			else
				intel_c20_write(i915, encoder->port, 0,
						PHY_C20_B_MPLLB_CNTX_CFG(i),
						pll_state->mpllb[i]);
		}
	}

	/* 4. Program custom width to match the link protocol */
	if (dp) {
		intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE0, PHY_C20_VDR_CUSTOM_WIDTH,
				is_dp2(pll_state->clock) ? 2 : 0,
				MB_WRITE_COMMITTED);
		if (lane_count == 4)
			intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE1, PHY_C20_VDR_CUSTOM_WIDTH,
					is_dp2(pll_state->clock) ? 2 : 0,
					MB_WRITE_COMMITTED);
	} else if (is_hdmi_frl(pll_state->clock)) {
		intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE0, PHY_C20_VDR_CUSTOM_WIDTH,
				1, MB_WRITE_COMMITTED);
		if (lane_count == 4)
			intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE1,
					PHY_C20_VDR_CUSTOM_WIDTH, 1, MB_WRITE_COMMITTED);
	} else
		intel_cx0_write(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C20_VDR_CUSTOM_WIDTH,
				0, MB_WRITE_COMMITTED);

	/* 5. For DP or 6. For HDMI */
	if (dp) {
		intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE0, PHY_C20_VDR_CUSTOM_SERDES_RATE,
				BIT(6) | (intel_c20_get_dp_rate(pll_state->clock) << 1),
				MB_WRITE_COMMITTED);
		if (lane_count == 4)
			intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE1, PHY_C20_VDR_CUSTOM_SERDES_RATE,
					BIT(6) | (intel_c20_get_dp_rate(pll_state->clock) << 1),
					MB_WRITE_COMMITTED);
	} else {
		intel_cx0_write(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C20_VDR_CUSTOM_SERDES_RATE,
				((is_hdmi_frl(pll_state->clock) ? 1 : 0) << 7),
				MB_WRITE_COMMITTED);

		intel_cx0_write(i915, encoder->port, INTEL_CX0_BOTH_LANES, PHY_C20_VDR_HDMI_RATE,
				(intel_c20_get_hdmi_rate(pll_state->clock) << 0),
				MB_WRITE_COMMITTED);
	}

	/*
	 * 7. Write Vendor specific registers to toggle context setting to load
	 * the updated programming toggle context bit
	 */
	intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE0, PHY_C20_VDR_CUSTOM_SERDES_RATE,
			cntx ? 0 : 1, MB_WRITE_COMMITTED);
	if (lane_count == 4)
		intel_cx0_write(i915, encoder->port, INTEL_CX0_LANE1, PHY_C20_VDR_CUSTOM_SERDES_RATE,
				cntx ? 0 : 1, MB_WRITE_COMMITTED);
}

int intel_c10mpllb_calc_port_clock(struct intel_encoder *encoder,
				   const struct intel_c10mpllb_state *pll_state)
{
	unsigned int frac_quot = 0, frac_rem = 0, frac_den = 1;
	unsigned int multiplier, tx_clk_div, hdmi_div, refclk = 38400;
	int tmpclk = 0;

	if (pll_state->pll[0] & C10_PLL0_FRACEN) {
		frac_quot = pll_state->pll[12] << 8 | pll_state->pll[11];
		frac_rem =  pll_state->pll[14] << 8 | pll_state->pll[13];
		frac_den =  pll_state->pll[10] << 8 | pll_state->pll[9];
	}

	multiplier = (REG_FIELD_GET8(C10_PLL3_MULTIPLIERH_MASK, pll_state->pll[3]) << 8 |
		      pll_state->pll[2]) / 2 + 16;

	tx_clk_div = REG_FIELD_GET8(C10_PLL15_TXCLKDIV_MASK, pll_state->pll[15]);
	hdmi_div = REG_FIELD_GET8(C10_PLL15_HDMIDIV_MASK, pll_state->pll[15]);

	tmpclk = DIV_ROUND_CLOSEST_ULL(mul_u32_u32(refclk, (multiplier << 16) + frac_quot) +
				     DIV_ROUND_CLOSEST(refclk * frac_rem, frac_den),
				     10 << (tx_clk_div + 16));
	tmpclk *= (hdmi_div ? 2 : 1);

	return tmpclk;
}

int intel_c20pll_calc_port_clock(struct intel_encoder *encoder,
				 const struct intel_c20pll_state *pll_state)
{
	unsigned int frac_quot = 0, frac_rem = 0, frac_den = 1;
	unsigned int multiplier = 0, tx_clk_div = 0, refclk = 38400;

	if (pll_state->mpllb[6] & C20_MPLLB_FRACEN) {
		frac_quot = pll_state->mpllb[8];
		frac_rem =  pll_state->mpllb[9];
		frac_den =  pll_state->mpllb[7];
		multiplier = REG_FIELD_GET(C20_MULTIPLIER_MASK, pll_state->mpllb[0]);
		tx_clk_div = REG_FIELD_GET(C20_MPLLB_TX_CLK_DIV_MASK, pll_state->mpllb[0]);
	} else if (pll_state->mplla[6] & C20_MPLLA_FRACEN) {
		frac_quot = pll_state->mplla[8];
		frac_rem =  pll_state->mplla[9];
		frac_den =  pll_state->mplla[7];
		multiplier = REG_FIELD_GET(C20_MULTIPLIER_MASK, pll_state->mplla[0]);
		tx_clk_div = REG_FIELD_GET(C20_MPLLA_TX_CLK_DIV_MASK, pll_state->mplla[1]);
       }

	return DIV_ROUND_CLOSEST_ULL(mul_u32_u32(refclk, (multiplier << 16) + frac_quot) +
	       DIV_ROUND_CLOSEST(refclk * frac_rem, frac_den),
				 10 << (tx_clk_div + 16));
}

static void intel_program_port_clock_ctl(struct intel_encoder *encoder,
					 const struct intel_crtc_state *crtc_state,
					 bool lane_reversal)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_dp *intel_dp;
	bool ssc_enabled;
	u32 val = 0;

	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL1(encoder->port), XELPDP_PORT_REVERSAL,
		     lane_reversal ? XELPDP_PORT_REVERSAL : 0);

	if (lane_reversal)
		val |= XELPDP_LANE1_PHY_CLOCK_SELECT;

	val |= XELPDP_FORWARD_CLOCK_UNGATE;

	if (is_hdmi_frl(crtc_state->port_clock))
		val |= XELPDP_DDI_CLOCK_SELECT(XELPDP_DDI_CLOCK_SELECT_DIV18CLK);
	else
		val |= XELPDP_DDI_CLOCK_SELECT(XELPDP_DDI_CLOCK_SELECT_MAXPCLK);

	if (intel_crtc_has_dp_encoder(crtc_state)) {
		intel_dp = enc_to_intel_dp(encoder);
		ssc_enabled = (intel_dp->dpcd[DP_MAX_DOWNSPREAD] &
			      DP_MAX_DOWNSPREAD_0_5);

		if (intel_dp_is_edp(intel_dp) && !intel_panel_use_ssc(i915))
			ssc_enabled = false;

		if (!intel_panel_use_ssc(i915))
			ssc_enabled = false;

		/* DP2.0 10G and 20G rates enable MPLLA*/
		if (crtc_state->port_clock == 1000000 || crtc_state->port_clock == 2000000) {
			val |= ssc_enabled ? XELPDP_SSC_ENABLE_PLLA : 0;
		} else {
			val |= ssc_enabled ? XELPDP_SSC_ENABLE_PLLB : 0;
		}
	}

	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_LANE1_PHY_CLOCK_SELECT | XELPDP_FORWARD_CLOCK_UNGATE |
		     XELPDP_DDI_CLOCK_SELECT_MASK |
		     XELPDP_SSC_ENABLE_PLLA | XELPDP_SSC_ENABLE_PLLB, val);
}

static u32 intel_cx0_get_powerdown_update(u8 lane)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_POWERDOWN_UPDATE;
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_POWERDOWN_UPDATE;
	else
		return XELPDP_LANE0_POWERDOWN_UPDATE |
		       XELPDP_LANE1_POWERDOWN_UPDATE;
}

static u32 intel_cx0_get_powerdown_state(u8 lane, u8 state)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_POWERDOWN_NEW_STATE(state);
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_POWERDOWN_NEW_STATE(state);
	else
		return XELPDP_LANE0_POWERDOWN_NEW_STATE(state) |
		       XELPDP_LANE1_POWERDOWN_NEW_STATE(state);
}

static void intel_cx0_powerdown_change_sequence(struct drm_i915_private *i915,
						enum port port,
						u8 lane, u8 state)
{
	enum phy phy = intel_port_to_phy(i915, port);

	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL2(port),
		     XELPDP_LANE0_POWERDOWN_NEW_STATE_MASK | XELPDP_LANE1_POWERDOWN_NEW_STATE_MASK,
		     intel_cx0_get_powerdown_state(lane, state));
	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL2(port),
		     XELPDP_LANE0_POWERDOWN_UPDATE | XELPDP_LANE1_POWERDOWN_UPDATE,
		     intel_cx0_get_powerdown_update(lane));

	/* Update Timeout Value */
	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_BUF_CTL2(port),
				      intel_cx0_get_powerdown_update(lane), 0,
				      XELPDP_PORT_POWERDOWN_UPDATE_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "PHY %c failed to bring out of Lane reset after %dus.\n",
			 phy_name(phy), XELPDP_PORT_RESET_START_TIMEOUT_US);
}

static void intel_cx0_setup_powerdown(struct drm_i915_private *i915, enum port port)
{
	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL2(port),
		     XELPDP_POWER_STATE_READY_MASK,
		     XELPDP_POWER_STATE_READY(CX0_P2_STATE_READY));
	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL3(port),
		     XELPDP_POWER_STATE_ACTIVE_MASK |
		     XELPDP_PLL_LANE_STAGGERING_DELAY_MASK,
		     XELPDP_POWER_STATE_ACTIVE(CX0_P0_STATE_ACTIVE) |
		     XELPDP_PLL_LANE_STAGGERING_DELAY(0));
}

static u32 intel_cx0_get_pclk_refclk_request(u8 lane)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_PCLK_REFCLK_REQUEST;
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_PCLK_REFCLK_REQUEST;
	else
		return XELPDP_LANE0_PCLK_REFCLK_REQUEST |
		       XELPDP_LANE1_PCLK_REFCLK_REQUEST;
}

static u32 intel_cx0_get_pclk_refclk_ack(u8 lane)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_PCLK_REFCLK_ACK;
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_PCLK_REFCLK_ACK;
	else
		return XELPDP_LANE0_PCLK_REFCLK_ACK |
		       XELPDP_LANE1_PCLK_REFCLK_ACK;
}

/* FIXME: Some Type-C cases need not reset both the lanes. Handle those cases. */
static void intel_cx0_phy_lane_reset(struct drm_i915_private *i915, enum port port,
				     bool lane_reversal)
{
	enum phy phy = intel_port_to_phy(i915, port);
	u8 lane = lane_reversal ? INTEL_CX0_LANE1 :
				  INTEL_CX0_LANE0;

	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_BUF_CTL1(port),
				      XELPDP_PORT_BUF_SOC_PHY_READY,
				      XELPDP_PORT_BUF_SOC_PHY_READY,
				      XELPDP_PORT_BUF_SOC_READY_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "PHY %c failed to bring out of SOC reset after %dus.\n",
			 phy_name(phy), XELPDP_PORT_BUF_SOC_READY_TIMEOUT_US);

	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL2(port),
		     XELPDP_LANE0_PIPE_RESET | XELPDP_LANE1_PIPE_RESET,
		     XELPDP_LANE0_PIPE_RESET | XELPDP_LANE1_PIPE_RESET);

	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_BUF_CTL2(port),
				      XELPDP_LANE0_PHY_CURRENT_STATUS | XELPDP_LANE1_PHY_CURRENT_STATUS,
				      XELPDP_LANE0_PHY_CURRENT_STATUS | XELPDP_LANE1_PHY_CURRENT_STATUS,
				      XELPDP_PORT_RESET_START_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "PHY %c failed to bring out of Lane reset after %dus.\n",
			 phy_name(phy), XELPDP_PORT_RESET_START_TIMEOUT_US);

	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(port),
		     intel_cx0_get_pclk_refclk_request(INTEL_CX0_BOTH_LANES),
		     intel_cx0_get_pclk_refclk_request(lane));

	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_CLOCK_CTL(port),
				      intel_cx0_get_pclk_refclk_ack(INTEL_CX0_BOTH_LANES),
				      intel_cx0_get_pclk_refclk_ack(lane),
				      XELPDP_REFCLK_ENABLE_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "PHY %c failed to request refclk after %dus.\n",
			 phy_name(phy), XELPDP_REFCLK_ENABLE_TIMEOUT_US);

	intel_cx0_powerdown_change_sequence(i915, port, INTEL_CX0_BOTH_LANES,
					    CX0_P2_STATE_RESET);
	intel_cx0_setup_powerdown(i915, port);

	intel_de_rmw(i915, XELPDP_PORT_BUF_CTL2(port),
		     XELPDP_LANE0_PIPE_RESET | XELPDP_LANE1_PIPE_RESET, 0);

	if (intel_de_wait_for_clear(i915, XELPDP_PORT_BUF_CTL2(port),
				    XELPDP_LANE0_PHY_CURRENT_STATUS |
				    XELPDP_LANE1_PHY_CURRENT_STATUS,
				    XELPDP_PORT_RESET_END_TIMEOUT))
		drm_warn(&i915->drm, "PHY %c failed to bring out of Lane reset after %dms.\n",
			 phy_name(phy), XELPDP_PORT_RESET_END_TIMEOUT);
}

static void intel_c10_program_phy_lane(struct drm_i915_private *i915,
				       struct intel_encoder *encoder, int lane_count,
				       bool lane_reversal)
{
	u8 l0t1, l0t2, l1t1, l1t2;
	bool dp_alt_mode = intel_tc_port_in_dp_alt_mode(enc_to_dig_port(encoder));
	enum port port = encoder->port;

	intel_cx0_rmw(i915, port, 1, PHY_C10_VDR_CONTROL(1),
		      C10_VDR_CTRL_MSGBUS_ACCESS | C10_VDR_CTRL_UPDATE_CFG,
		      C10_VDR_CTRL_MSGBUS_ACCESS, MB_WRITE_COMMITTED);
	intel_cx0_rmw(i915, port, 0, PHY_C10_VDR_CONTROL(1),
		      C10_VDR_CTRL_MSGBUS_ACCESS | C10_VDR_CTRL_UPDATE_CFG,
		      C10_VDR_CTRL_MASTER_LANE  | C10_VDR_CTRL_MSGBUS_ACCESS, MB_WRITE_COMMITTED);

	l0t1 = intel_cx0_read(i915, port, 0, PHY_CX0_TX_CONTROL(1, 2));
	l0t2 = intel_cx0_read(i915, port, 0, PHY_CX0_TX_CONTROL(2, 2));
	l1t1 = intel_cx0_read(i915, port, 1, PHY_CX0_TX_CONTROL(1, 2));
	l1t2 = intel_cx0_read(i915, port, 1, PHY_CX0_TX_CONTROL(2, 2));

	if (lane_reversal) {
		switch (lane_count) {
		case 1:
			/* Disable MLs 1(lane0), 2(lane0), 3(lane1) */
			intel_cx0_write(i915, port, 1, PHY_CX0_TX_CONTROL(1, 2),
					l1t1 | CONTROL2_DISABLE_SINGLE_TX,
					MB_WRITE_COMMITTED);
			fallthrough;
		case 2:
			/* Disable MLs 1(lane0), 2(lane0) */
			intel_cx0_write(i915, port, 0, PHY_CX0_TX_CONTROL(2, 2),
					l0t2 | CONTROL2_DISABLE_SINGLE_TX,
					MB_WRITE_COMMITTED);
			fallthrough;
		case 3:
			/* Disable MLs 1(lane0) */
			intel_cx0_write(i915, port, 0, PHY_CX0_TX_CONTROL(1, 2),
					l0t1 | CONTROL2_DISABLE_SINGLE_TX,
					MB_WRITE_COMMITTED);
			break;
		}
	} else {
		switch (lane_count) {
		case 1:
			if (dp_alt_mode) {
				/* Disable MLs 1(lane0), 3(lane1), 4(lane1) */
				intel_cx0_write(i915, port, 0, PHY_CX0_TX_CONTROL(1, 2),
						l0t1 | CONTROL2_DISABLE_SINGLE_TX,
						MB_WRITE_COMMITTED);
			} else {
				/* Disable MLs 2(lane0), 3(lane1), 4(lane1) */
				intel_cx0_write(i915, port, 0, PHY_CX0_TX_CONTROL(2, 2),
						l0t2 | CONTROL2_DISABLE_SINGLE_TX,
						MB_WRITE_COMMITTED);
			}
			fallthrough;
		case 2:
			/* Disable MLs 3(lane1), 4(lane1) */
			intel_cx0_write(i915, port, 1, PHY_CX0_TX_CONTROL(1, 2),
					l1t1 | CONTROL2_DISABLE_SINGLE_TX,
					MB_WRITE_COMMITTED);
			fallthrough;
		case 3:
			/* Disable MLs 4(lane1) */
			intel_cx0_write(i915, port, 1, PHY_CX0_TX_CONTROL(2, 2),
					l1t2 | CONTROL2_DISABLE_SINGLE_TX,
					MB_WRITE_COMMITTED);
			break;
		}
	}

	if (intel_is_c10phy(i915, intel_port_to_phy(i915, port))) {
		intel_cx0_rmw(i915, port, 1, PHY_C10_VDR_CONTROL(1),
			      C10_VDR_CTRL_UPDATE_CFG | C10_VDR_CTRL_MSGBUS_ACCESS,
			      C10_VDR_CTRL_UPDATE_CFG, MB_WRITE_COMMITTED);
		intel_cx0_rmw(i915, port, 0, PHY_C10_VDR_CONTROL(1),
			      C10_VDR_CTRL_UPDATE_CFG | C10_VDR_CTRL_MSGBUS_ACCESS,
			      C10_VDR_CTRL_MASTER_LANE | C10_VDR_CTRL_UPDATE_CFG, MB_WRITE_COMMITTED);
	}
}

static u32 intel_cx0_get_pclk_pll_request(u8 lane)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_PCLK_PLL_REQUEST;
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_PCLK_PLL_REQUEST;
	else
		return XELPDP_LANE0_PCLK_PLL_REQUEST |
		       XELPDP_LANE1_PCLK_PLL_REQUEST;
}

static u32 intel_cx0_get_pclk_pll_ack(u8 lane)
{
	if (lane == INTEL_CX0_LANE0)
		return XELPDP_LANE0_PCLK_PLL_ACK;
	else if (lane == INTEL_CX0_LANE1)
		return XELPDP_LANE1_PCLK_PLL_ACK;
	else
		return XELPDP_LANE0_PCLK_PLL_ACK |
		       XELPDP_LANE1_PCLK_PLL_ACK;
}

static void intel_cx0pll_enable(struct intel_encoder *encoder,
				const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	bool lane_reversal = dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL;
	u8 maxpclk_lane = lane_reversal ? INTEL_CX0_LANE1 :
					  INTEL_CX0_LANE0;
	intel_wakeref_t wakeref = intel_cx0_phy_transaction_begin(encoder);

	/*
	 * 1. Program PORT_CLOCK_CTL REGISTER to configure
	 * clock muxes, gating and SSC
	 */
	intel_program_port_clock_ctl(encoder, crtc_state, lane_reversal);

	/* 2. Bring PHY out of reset. */
	intel_cx0_phy_lane_reset(i915, encoder->port, lane_reversal);

	/*
	 * 3. Change Phy power state to Ready.
	 * TODO: For DP alt mode use only one lane.
	 */
	intel_cx0_powerdown_change_sequence(i915, encoder->port, INTEL_CX0_BOTH_LANES,
					    CX0_P2_STATE_READY);

	/* 4. Program PHY internal PLL internal registers. */
	if (intel_is_c10phy(i915, phy))
		intel_c10_pll_program(i915, crtc_state, encoder);
	else
		intel_c20_pll_program(i915, crtc_state, encoder);

	/*
	 * 5. Program the enabled and disabled owned PHY lane
	 * transmitters over message bus
	 */
	intel_c10_program_phy_lane(i915, encoder, crtc_state->lane_count, lane_reversal);

	/*
	 * 6. Follow the Display Voltage Frequency Switching - Sequence
	 * Before Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 7. Program DDI_CLK_VALFREQ to match intended DDI
	 * clock frequency.
	 */
	intel_de_write(i915, DDI_CLK_VALFREQ(encoder->port),
		       crtc_state->port_clock);
	/*
	 * 8. Set PORT_CLOCK_CTL register PCLK PLL Request
	 * LN<Lane for maxPCLK> to "1" to enable PLL.
	 */
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     intel_cx0_get_pclk_pll_request(INTEL_CX0_BOTH_LANES),
		     intel_cx0_get_pclk_pll_request(maxpclk_lane));

	/* 9. Poll on PORT_CLOCK_CTL PCLK PLL Ack LN<Lane for maxPCLK> == "1". */
	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_CLOCK_CTL(encoder->port),
				      intel_cx0_get_pclk_pll_ack(INTEL_CX0_BOTH_LANES),
				      intel_cx0_get_pclk_pll_ack(maxpclk_lane),
				      XELPDP_PCLK_PLL_ENABLE_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "Port %c PLL not locked after %dus.\n",
			 phy_name(phy), XELPDP_PCLK_PLL_ENABLE_TIMEOUT_US);

	/*
	 * 10. Follow the Display Voltage Frequency Switching Sequence After
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	intel_cx0_phy_transaction_end(encoder, wakeref);
}

int intel_mtl_tbt_calc_port_clock(struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	u32 clock;
	u32 val = intel_de_read(i915, XELPDP_PORT_CLOCK_CTL(encoder->port));

	clock = REG_FIELD_GET(XELPDP_DDI_CLOCK_SELECT_MASK, val);

	drm_WARN_ON(&i915->drm, !(val & XELPDP_FORWARD_CLOCK_UNGATE));
	drm_WARN_ON(&i915->drm, !(val & XELPDP_TBT_CLOCK_REQUEST));
	drm_WARN_ON(&i915->drm, !(val & XELPDP_TBT_CLOCK_ACK));

	switch (clock) {
	case XELPDP_DDI_CLOCK_SELECT_TBT_162:
		return 162000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_270:
		return 270000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_540:
		return 540000;
	case XELPDP_DDI_CLOCK_SELECT_TBT_810:
		return 810000;
	default:
		MISSING_CASE(clock);
		return 162000;
       }
}

static int intel_mtl_tbt_clock_select(struct drm_i915_private *i915, int clock)
{
	switch (clock) {
	case 162000:
		return XELPDP_DDI_CLOCK_SELECT_TBT_162;
	case 270000:
		return XELPDP_DDI_CLOCK_SELECT_TBT_270;
	case 540000:
		return XELPDP_DDI_CLOCK_SELECT_TBT_540;
	case 810000:
		return XELPDP_DDI_CLOCK_SELECT_TBT_810;
	default:
		MISSING_CASE(clock);
		return XELPDP_DDI_CLOCK_SELECT_TBT_162;
       }
}

static void intel_mtl_tbt_pll_enable(struct intel_encoder *encoder,
				     const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	u32 val = 0;

	/*
	 * 1. Program PORT_CLOCK_CTL REGISTER to configure
	 * clock muxes, gating and SSC
	 */
	val |= XELPDP_DDI_CLOCK_SELECT(intel_mtl_tbt_clock_select(i915, crtc_state->port_clock));
	val |= XELPDP_FORWARD_CLOCK_UNGATE;
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_DDI_CLOCK_SELECT_MASK | XELPDP_FORWARD_CLOCK_UNGATE, val);

	/* 2. Read back PORT_CLOCK_CTL REGISTER */
	val = intel_de_read(i915, XELPDP_PORT_CLOCK_CTL(encoder->port));

	/*
	 * 3. Follow the Display Voltage Frequency Switching - Sequence
	 * Before Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 4. Set PORT_CLOCK_CTL register TBT CLOCK Request to "1" to enable PLL.
	 */
	val |= XELPDP_TBT_CLOCK_REQUEST;
	intel_de_write(i915, XELPDP_PORT_CLOCK_CTL(encoder->port), val);

	/* 5. Poll on PORT_CLOCK_CTL TBT CLOCK Ack == "1". */
	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_CLOCK_CTL(encoder->port),
				      XELPDP_TBT_CLOCK_ACK,
				      XELPDP_TBT_CLOCK_ACK,
				      100, 0, NULL))
		drm_warn(&i915->drm, "[ENCODER:%d:%s][%c] PHY PLL not locked after 100us.\n",
			 encoder->base.base.id, encoder->base.name, phy_name(phy));

	/*
	 * 6. Follow the Display Voltage Frequency Switching Sequence After
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 7. Program DDI_CLK_VALFREQ to match intended DDI
	 * clock frequency.
	 */
	intel_de_write(i915, DDI_CLK_VALFREQ(encoder->port),
		       crtc_state->port_clock);
}

void intel_mtl_pll_enable(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);

	if (intel_tc_port_in_tbt_alt_mode(dig_port))
		intel_mtl_tbt_pll_enable(encoder, crtc_state);
	else
		intel_cx0pll_enable(encoder, crtc_state);
}

static void intel_cx0pll_disable(struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);
	bool is_c10 = intel_is_c10phy(i915, phy);
	intel_wakeref_t wakeref = intel_cx0_phy_transaction_begin(encoder);

	/* 1. Change owned PHY lane power to Disable state. */
	intel_cx0_powerdown_change_sequence(i915, encoder->port, INTEL_CX0_BOTH_LANES,
					    is_c10 ? CX0_P2PG_STATE_DISABLE :
					    CX0_P4PG_STATE_DISABLE);

	/*
	 * 2. Follow the Display Voltage Frequency Switching Sequence Before
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 3. Set PORT_CLOCK_CTL register PCLK PLL Request LN<Lane for maxPCLK>
	 * to "0" to disable PLL.
	 */
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     intel_cx0_get_pclk_pll_request(INTEL_CX0_BOTH_LANES) |
		     intel_cx0_get_pclk_refclk_request(INTEL_CX0_BOTH_LANES), 0);

	/* 4. Program DDI_CLK_VALFREQ to 0. */
	intel_de_write(i915, DDI_CLK_VALFREQ(encoder->port), 0);

	/*
	 * 5. Poll on PORT_CLOCK_CTL PCLK PLL Ack LN<Lane for maxPCLK**> == "0".
	 */
	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_CLOCK_CTL(encoder->port),
				      intel_cx0_get_pclk_pll_ack(INTEL_CX0_BOTH_LANES) |
				      intel_cx0_get_pclk_refclk_ack(INTEL_CX0_BOTH_LANES), 0,
				      XELPDP_PCLK_PLL_DISABLE_TIMEOUT_US, 0, NULL))
		drm_warn(&i915->drm, "Port %c PLL not unlocked after %dus.\n",
			 phy_name(phy), XELPDP_PCLK_PLL_DISABLE_TIMEOUT_US);

	/*
	 * 6. Follow the Display Voltage Frequency Switching Sequence After
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/* 7. Program PORT_CLOCK_CTL register to disable and gate clocks. */
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_DDI_CLOCK_SELECT_MASK, 0);
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_FORWARD_CLOCK_UNGATE, 0);

	intel_cx0_phy_transaction_end(encoder, wakeref);
}

static void intel_mtl_tbt_pll_disable(struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);

	/*
	 * 1. Follow the Display Voltage Frequency Switching Sequence Before
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 2. Set PORT_CLOCK_CTL register TBT CLOCK Request to "0" to disable PLL.
	 */
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_TBT_CLOCK_REQUEST, 0);

	/* 3. Poll on PORT_CLOCK_CTL TBT CLOCK Ack == "0". */
	if (__intel_wait_for_register(&i915->uncore, XELPDP_PORT_CLOCK_CTL(encoder->port),
				      XELPDP_TBT_CLOCK_ACK,
				      ~XELPDP_TBT_CLOCK_ACK,
				      10, 0, NULL))
		drm_warn(&i915->drm, "[ENCODER:%d:%s][%c] PHY PLL not unlocked after 10us.\n",
			 encoder->base.base.id, encoder->base.name, phy_name(phy));

	/*
	 * 4. Follow the Display Voltage Frequency Switching Sequence After
	 * Frequency Change. We handle this step in bxt_set_cdclk().
	 */

	/*
	 * 5. Program PORT CLOCK CTRL register to disable and gate clocks
	 */
	intel_de_rmw(i915, XELPDP_PORT_CLOCK_CTL(encoder->port),
		     XELPDP_DDI_CLOCK_SELECT_MASK |
		     XELPDP_FORWARD_CLOCK_UNGATE, 0);

	/* 6. Program DDI_CLK_VALFREQ to 0. */
	intel_de_write(i915, DDI_CLK_VALFREQ(encoder->port), 0);
}

void intel_mtl_pll_disable(struct intel_encoder *encoder)
{
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);

	if (intel_tc_port_in_tbt_alt_mode(dig_port))
		intel_mtl_tbt_pll_disable(encoder);
	else
		intel_cx0pll_disable(encoder);
}

void intel_c10mpllb_state_verify(struct intel_atomic_state *state,
				 struct intel_crtc_state *new_crtc_state)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	struct intel_c10mpllb_state mpllb_hw_state = { 0 };
	struct intel_c10mpllb_state *mpllb_sw_state = &new_crtc_state->cx0pll_state.c10mpllb_state;
	struct intel_crtc *crtc = to_intel_crtc(new_crtc_state->uapi.crtc);
	struct intel_encoder *encoder;
	struct intel_dp *intel_dp;
	enum phy phy;
	int i;
	bool use_ssc = false;
	bool use_hdmi = false;

	if (DISPLAY_VER(i915) < 14)
		return;

	if (!new_crtc_state->hw.active)
		return;

	encoder = intel_get_crtc_new_encoder(state, new_crtc_state);
	phy = intel_port_to_phy(i915, encoder->port);

	if (intel_crtc_has_dp_encoder(new_crtc_state)) {
		intel_dp = enc_to_intel_dp(encoder);
		use_ssc = (intel_dp->dpcd[DP_MAX_DOWNSPREAD] &
			  DP_MAX_DOWNSPREAD_0_5);

		if (!intel_panel_use_ssc(i915))
			use_ssc = false;
	} else {
		use_hdmi = true;
	}

	if (!intel_is_c10phy(i915, phy))
		return;

	intel_c10mpllb_readout_hw_state(encoder, &mpllb_hw_state);

	for (i = 0; i < ARRAY_SIZE(mpllb_sw_state->pll); i++) {
		u8 expected;

		if (!(use_ssc || use_hdmi) && i > 3 && i < 9)
			expected = 0;
		else
			expected = mpllb_sw_state->pll[i];

		I915_STATE_WARN(mpllb_hw_state.pll[i] != expected,
				"[CRTC:%d:%s] mismatch in C10MPLLB: Register[%d] (expected 0x%02x, found 0x%02x)",
				crtc->base.base.id, crtc->base.name,
				i, expected, mpllb_hw_state.pll[i]);
	}
}
