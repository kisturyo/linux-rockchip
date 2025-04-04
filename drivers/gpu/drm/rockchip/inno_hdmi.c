// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 *    Zheng Yang <zhengyang@rock-chips.com>
 *    Yakir Yang <ykk@rock-chips.com>
 */

#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hdmi.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include <sound/hdmi-codec.h>

#include "rockchip_drm_drv.h"
#include "rockchip_drm_vop.h"

#include "inno_hdmi.h"

#define to_inno_hdmi(x)	container_of(x, struct inno_hdmi, x)

struct audio_info {
	int sample_rate;
	int channels;
	int sample_width;
};

struct hdmi_data_info {
	int vic;
	bool sink_has_audio;
	unsigned int enc_in_format;
	unsigned int enc_out_format;
	unsigned int colorimetry;
};

struct inno_hdmi_i2c {
	struct i2c_adapter adap;

	u8 ddc_addr;
	u8 segment_addr;

	struct mutex lock;
	struct completion cmp;
};

enum inno_hdmi_dev_type {
	RK3036_HDMI,
	RK3128_HDMI,
};

struct inno_hdmi_phy_config {
	unsigned long mpixelclock;
	u8 pre_emphasis;	/* pre-emphasis value */
	u8 vlev_ctr;		/* voltage level control */
};

struct inno_hdmi_plat_data {
	enum inno_hdmi_dev_type dev_type;
	struct inno_hdmi_phy_config *phy_config;
};

struct inno_hdmi {
	struct device *dev;
	struct drm_device *drm_dev;

	int irq;
	struct clk *aclk;
	struct clk *pclk;
	void __iomem *regs;

	struct drm_connector	connector;
	struct drm_encoder	encoder;

	struct inno_hdmi_i2c *i2c;
	struct i2c_adapter *ddc;

	unsigned int tmds_rate;
	struct platform_device *audio_pdev;
	bool audio_enable;
	const struct inno_hdmi_plat_data *plat_data;

	struct hdmi_data_info	hdmi_data;
	struct drm_display_mode previous_mode;
};

static struct inno_hdmi *connector_to_inno_hdmi(struct drm_connector *connector)
{
	return container_of(connector, struct inno_hdmi, connector);
}

enum {
	CSC_ITU601_16_235_TO_RGB_0_255_8BIT,
	CSC_ITU601_0_255_TO_RGB_0_255_8BIT,
	CSC_ITU709_16_235_TO_RGB_0_255_8BIT,
	CSC_RGB_0_255_TO_ITU601_16_235_8BIT,
	CSC_RGB_0_255_TO_ITU709_16_235_8BIT,
	CSC_RGB_0_255_TO_RGB_16_235_8BIT,
};

static const char coeff_csc[][24] = {
	/*
	 * YUV2RGB:601 SD mode(Y[16:235], UV[16:240], RGB[0:255]):
	 *   R = 1.164*Y + 1.596*V - 204
	 *   G = 1.164*Y - 0.391*U - 0.813*V + 154
	 *   B = 1.164*Y + 2.018*U - 258
	 */
	{
		0x04, 0xa7, 0x00, 0x00, 0x06, 0x62, 0x02, 0xcc,
		0x04, 0xa7, 0x11, 0x90, 0x13, 0x40, 0x00, 0x9a,
		0x04, 0xa7, 0x08, 0x12, 0x00, 0x00, 0x03, 0x02
	},
	/*
	 * YUV2RGB:601 SD mode(YUV[0:255],RGB[0:255]):
	 *   R = Y + 1.402*V - 248
	 *   G = Y - 0.344*U - 0.714*V + 135
	 *   B = Y + 1.772*U - 227
	 */
	{
		0x04, 0x00, 0x00, 0x00, 0x05, 0x9b, 0x02, 0xf8,
		0x04, 0x00, 0x11, 0x60, 0x12, 0xdb, 0x00, 0x87,
		0x04, 0x00, 0x07, 0x16, 0x00, 0x00, 0x02, 0xe3
	},
	/*
	 * YUV2RGB:709 HD mode(Y[16:235],UV[16:240],RGB[0:255]):
	 *   R = 1.164*Y + 1.793*V - 248
	 *   G = 1.164*Y - 0.213*U - 0.534*V + 77
	 *   B = 1.164*Y + 2.115*U - 289
	 */
	{
		0x04, 0xa7, 0x00, 0x00, 0x07, 0x2c, 0x02, 0xf8,
		0x04, 0xa7, 0x10, 0xda, 0x12, 0x22, 0x00, 0x4d,
		0x04, 0xa7, 0x08, 0x74, 0x00, 0x00, 0x03, 0x21
	},

	/*
	 * RGB2YUV:601 SD mode:
	 *   Cb = -0.291G - 0.148R + 0.439B + 128
	 *   Y  = 0.504G  + 0.257R + 0.098B + 16
	 *   Cr = -0.368G + 0.439R - 0.071B + 128
	 */
	{
		0x11, 0x5f, 0x01, 0x82, 0x10, 0x23, 0x00, 0x80,
		0x02, 0x1c, 0x00, 0xa1, 0x00, 0x36, 0x00, 0x1e,
		0x11, 0x29, 0x10, 0x59, 0x01, 0x82, 0x00, 0x80
	},
	/*
	 * RGB2YUV:709 HD mode:
	 *   Cb = - 0.338G - 0.101R + 0.439B + 128
	 *   Y  = 0.614G   + 0.183R + 0.062B + 16
	 *   Cr = - 0.399G + 0.439R - 0.040B + 128
	 */
	{
		0x11, 0x98, 0x01, 0xc1, 0x10, 0x28, 0x00, 0x80,
		0x02, 0x74, 0x00, 0xbb, 0x00, 0x3f, 0x00, 0x10,
		0x11, 0x5a, 0x10, 0x67, 0x01, 0xc1, 0x00, 0x80
	},
	/*
	 * RGB[0:255]2RGB[16:235]:
	 *   R' = R x (235-16)/255 + 16;
	 *   G' = G x (235-16)/255 + 16;
	 *   B' = B x (235-16)/255 + 16;
	 */
	{
		0x00, 0x00, 0x03, 0x6F, 0x00, 0x00, 0x00, 0x10,
		0x03, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
		0x00, 0x00, 0x00, 0x00, 0x03, 0x6F, 0x00, 0x10
	},
};

static inline u8 hdmi_readb(struct inno_hdmi *hdmi, u16 offset)
{
	return readl_relaxed(hdmi->regs + (offset) * 0x04);
}

static inline void hdmi_writeb(struct inno_hdmi *hdmi, u16 offset, u32 val)
{
	writel_relaxed(val, hdmi->regs + (offset) * 0x04);
}

static inline void hdmi_modb(struct inno_hdmi *hdmi, u16 offset,
			     u32 msk, u32 val)
{
	u8 temp = hdmi_readb(hdmi, offset) & ~msk;

	temp |= val & msk;
	hdmi_writeb(hdmi, offset, temp);
}

static void inno_hdmi_i2c_init(struct inno_hdmi *hdmi)
{
	int ddc_bus_freq;

	ddc_bus_freq = (hdmi->tmds_rate >> 2) / HDMI_SCL_RATE;

	hdmi_writeb(hdmi, DDC_BUS_FREQ_L, ddc_bus_freq & 0xFF);
	hdmi_writeb(hdmi, DDC_BUS_FREQ_H, (ddc_bus_freq >> 8) & 0xFF);

	/* Clear the EDID interrupt flag and mute the interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, 0);
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);
}

static void inno_hdmi_sys_power(struct inno_hdmi *hdmi, bool enable)
{
	if (enable)
		hdmi_modb(hdmi, HDMI_SYS_CTRL, m_POWER, v_PWR_ON);
	else
		hdmi_modb(hdmi, HDMI_SYS_CTRL, m_POWER, v_PWR_OFF);
}

static void inno_hdmi_set_pwr_mode(struct inno_hdmi *hdmi, int mode)
{
	const struct inno_hdmi_phy_config *phy_config =
						hdmi->plat_data->phy_config;

	switch (mode) {
	case NORMAL:
		inno_hdmi_sys_power(hdmi, false);
		for (; phy_config->mpixelclock != ~0UL; phy_config++)
			if (hdmi->tmds_rate <= phy_config->mpixelclock)
				break;
		if (!phy_config->mpixelclock)
			return;
		hdmi_writeb(hdmi, HDMI_PHY_PRE_EMPHASIS,
			    phy_config->pre_emphasis);
		hdmi_writeb(hdmi, HDMI_PHY_DRIVER, phy_config->vlev_ctr);

		hdmi_writeb(hdmi, HDMI_PHY_SYS_CTL, 0x15);
		hdmi_writeb(hdmi, HDMI_PHY_SYS_CTL, 0x14);
		hdmi_writeb(hdmi, HDMI_PHY_SYS_CTL, 0x10);
		hdmi_writeb(hdmi, HDMI_PHY_CHG_PWR, 0x0f);
		hdmi_writeb(hdmi, HDMI_PHY_SYNC, 0x00);
		hdmi_writeb(hdmi, HDMI_PHY_SYNC, 0x01);

		inno_hdmi_sys_power(hdmi, true);
		break;

	case LOWER_PWR:
		inno_hdmi_sys_power(hdmi, false);
		hdmi_writeb(hdmi, HDMI_PHY_DRIVER, 0x00);
		hdmi_writeb(hdmi, HDMI_PHY_PRE_EMPHASIS, 0x00);
		hdmi_writeb(hdmi, HDMI_PHY_CHG_PWR, 0x00);
		hdmi_writeb(hdmi, HDMI_PHY_SYS_CTL, 0x15);

		break;

	default:
		DRM_DEV_ERROR(hdmi->dev, "Unknown power mode %d\n", mode);
	}
}

static void inno_hdmi_reset(struct inno_hdmi *hdmi)
{
	u32 val;
	u32 msk;

	hdmi_modb(hdmi, HDMI_SYS_CTRL, m_RST_DIGITAL, v_NOT_RST_DIGITAL);
	udelay(100);

	hdmi_modb(hdmi, HDMI_SYS_CTRL, m_RST_ANALOG, v_NOT_RST_ANALOG);
	udelay(100);

	msk = m_REG_CLK_INV | m_REG_CLK_SOURCE | m_POWER | m_INT_POL;
	val = v_REG_CLK_INV | v_REG_CLK_SOURCE_SYS | v_PWR_ON | v_INT_POL_HIGH;
	hdmi_modb(hdmi, HDMI_SYS_CTRL, msk, val);

	inno_hdmi_set_pwr_mode(hdmi, NORMAL);
}

static int inno_hdmi_upload_frame(struct inno_hdmi *hdmi, int setup_rc,
				  union hdmi_infoframe *frame, u32 frame_index,
				  u32 mask, u32 disable, u32 enable)
{
	if (mask)
		hdmi_modb(hdmi, HDMI_PACKET_SEND_AUTO, mask, disable);

	hdmi_writeb(hdmi, HDMI_CONTROL_PACKET_BUF_INDEX, frame_index);

	if (setup_rc >= 0) {
		u8 packed_frame[HDMI_MAXIMUM_INFO_FRAME_SIZE];
		ssize_t rc, i;

		rc = hdmi_infoframe_pack(frame, packed_frame,
					 sizeof(packed_frame));
		if (rc < 0)
			return rc;

		for (i = 0; i < rc; i++)
			hdmi_writeb(hdmi, HDMI_CONTROL_PACKET_ADDR + i,
				    packed_frame[i]);

		if (mask)
			hdmi_modb(hdmi, HDMI_PACKET_SEND_AUTO, mask, enable);
	}

	return setup_rc;
}

static int inno_hdmi_config_video_vsi(struct inno_hdmi *hdmi,
				      struct drm_display_mode *mode)
{
	union hdmi_infoframe frame;
	int rc;

	rc = drm_hdmi_vendor_infoframe_from_display_mode(&frame.vendor.hdmi,
							 &hdmi->connector,
							 mode);

	return inno_hdmi_upload_frame(hdmi, rc, &frame, INFOFRAME_VSI,
		m_PACKET_VSI_EN, v_PACKET_VSI_EN(0), v_PACKET_VSI_EN(1));
}

static int inno_hdmi_config_audio_aai(struct inno_hdmi *hdmi,
				      struct audio_info *audio)
{
	struct hdmi_audio_infoframe *faudio;
	union hdmi_infoframe frame;
	int rc;

	rc = hdmi_audio_infoframe_init(&frame.audio);
	faudio = (struct hdmi_audio_infoframe *)&frame;

	faudio->channels = audio->channels;

	return inno_hdmi_upload_frame(hdmi, rc, &frame, INFOFRAME_AAI, 0, 0, 0);
}

static int inno_hdmi_config_video_avi(struct inno_hdmi *hdmi,
				      struct drm_display_mode *mode)
{
	union hdmi_infoframe frame;
	int rc;

	rc = drm_hdmi_avi_infoframe_from_display_mode(&frame.avi,
						      &hdmi->connector,
						      mode);

	if (hdmi->hdmi_data.enc_out_format == HDMI_COLORSPACE_YUV444)
		frame.avi.colorspace = HDMI_COLORSPACE_YUV444;
	else if (hdmi->hdmi_data.enc_out_format == HDMI_COLORSPACE_YUV422)
		frame.avi.colorspace = HDMI_COLORSPACE_YUV422;
	else
		frame.avi.colorspace = HDMI_COLORSPACE_RGB;

	return inno_hdmi_upload_frame(hdmi, rc, &frame, INFOFRAME_AVI, 0, 0, 0);
}

static int inno_hdmi_config_video_csc(struct inno_hdmi *hdmi)
{
	struct hdmi_data_info *data = &hdmi->hdmi_data;
	int c0_c2_change = 0;
	int csc_enable = 0;
	int csc_mode = 0;
	int auto_csc = 0;
	int value;
	int i;

	/* Input video mode is SDR RGB24bit, data enable signal from external */
	hdmi_writeb(hdmi, HDMI_VIDEO_CONTRL1, v_DE_EXTERNAL |
		    v_VIDEO_INPUT_FORMAT(VIDEO_INPUT_SDR_RGB444));

	/* Input color hardcode to RGB, and output color hardcode to RGB888 */
	value = v_VIDEO_INPUT_BITS(VIDEO_INPUT_8BITS) |
		v_VIDEO_OUTPUT_COLOR(0) |
		v_VIDEO_INPUT_CSP(0);
	hdmi_writeb(hdmi, HDMI_VIDEO_CONTRL2, value);

	if (data->enc_in_format == data->enc_out_format) {
		if ((data->enc_in_format == HDMI_COLORSPACE_RGB) ||
		    (data->enc_in_format >= HDMI_COLORSPACE_YUV444)) {
			value = v_SOF_DISABLE | v_COLOR_DEPTH_NOT_INDICATED(1);
			hdmi_writeb(hdmi, HDMI_VIDEO_CONTRL3, value);

			hdmi_modb(hdmi, HDMI_VIDEO_CONTRL,
				  m_VIDEO_AUTO_CSC | m_VIDEO_C0_C2_SWAP,
				  v_VIDEO_AUTO_CSC(AUTO_CSC_DISABLE) |
				  v_VIDEO_C0_C2_SWAP(C0_C2_CHANGE_DISABLE));
			return 0;
		}
	}

	if (data->colorimetry == HDMI_COLORIMETRY_ITU_601) {
		if ((data->enc_in_format == HDMI_COLORSPACE_RGB) &&
		    (data->enc_out_format == HDMI_COLORSPACE_YUV444)) {
			csc_mode = CSC_RGB_0_255_TO_ITU601_16_235_8BIT;
			auto_csc = AUTO_CSC_DISABLE;
			c0_c2_change = C0_C2_CHANGE_DISABLE;
			csc_enable = v_CSC_ENABLE;
		} else if ((data->enc_in_format == HDMI_COLORSPACE_YUV444) &&
			   (data->enc_out_format == HDMI_COLORSPACE_RGB)) {
			csc_mode = CSC_ITU601_16_235_TO_RGB_0_255_8BIT;
			auto_csc = AUTO_CSC_ENABLE;
			c0_c2_change = C0_C2_CHANGE_DISABLE;
			csc_enable = v_CSC_DISABLE;
		}
	} else {
		if ((data->enc_in_format == HDMI_COLORSPACE_RGB) &&
		    (data->enc_out_format == HDMI_COLORSPACE_YUV444)) {
			csc_mode = CSC_RGB_0_255_TO_ITU709_16_235_8BIT;
			auto_csc = AUTO_CSC_DISABLE;
			c0_c2_change = C0_C2_CHANGE_DISABLE;
			csc_enable = v_CSC_ENABLE;
		} else if ((data->enc_in_format == HDMI_COLORSPACE_YUV444) &&
			   (data->enc_out_format == HDMI_COLORSPACE_RGB)) {
			csc_mode = CSC_ITU709_16_235_TO_RGB_0_255_8BIT;
			auto_csc = AUTO_CSC_ENABLE;
			c0_c2_change = C0_C2_CHANGE_DISABLE;
			csc_enable = v_CSC_DISABLE;
		}
	}

	for (i = 0; i < 24; i++)
		hdmi_writeb(hdmi, HDMI_VIDEO_CSC_COEF + i,
			    coeff_csc[csc_mode][i]);

	value = v_SOF_DISABLE | csc_enable | v_COLOR_DEPTH_NOT_INDICATED(1);
	hdmi_writeb(hdmi, HDMI_VIDEO_CONTRL3, value);
	hdmi_modb(hdmi, HDMI_VIDEO_CONTRL, m_VIDEO_AUTO_CSC |
		  m_VIDEO_C0_C2_SWAP, v_VIDEO_AUTO_CSC(auto_csc) |
		  v_VIDEO_C0_C2_SWAP(c0_c2_change));

	return 0;
}

static int inno_hdmi_config_video_timing(struct inno_hdmi *hdmi,
					 struct drm_display_mode *mode)
{
	int value;

	if (hdmi->plat_data->dev_type == RK3036_HDMI) {
		value = BIT(20) | BIT(21);
		value |= mode->flags & DRM_MODE_FLAG_PHSYNC ? BIT(4) : 0;
		value |= mode->flags & DRM_MODE_FLAG_PVSYNC ? BIT(5) : 0;
		hdmi_writeb(hdmi, 0x148, value);
	}
	/* Set detail external video timing polarity and interlace mode */
	value = v_EXTERANL_VIDEO(1);
	value |= mode->flags & DRM_MODE_FLAG_PHSYNC ?
		 v_HSYNC_POLARITY(1) : v_HSYNC_POLARITY(0);
	value |= mode->flags & DRM_MODE_FLAG_PVSYNC ?
		 v_VSYNC_POLARITY(1) : v_VSYNC_POLARITY(0);
	value |= mode->flags & DRM_MODE_FLAG_INTERLACE ?
		 v_INETLACE(1) : v_INETLACE(0);
	hdmi_writeb(hdmi, HDMI_VIDEO_TIMING_CTL, value);

	/* Set detail external video timing */
	value = mode->htotal;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HTOTAL_L, value & 0xFF);
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HTOTAL_H, (value >> 8) & 0xFF);

	value = mode->htotal - mode->hdisplay;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HBLANK_L, value & 0xFF);
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HBLANK_H, (value >> 8) & 0xFF);

	value = mode->htotal - mode->hsync_start;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HDELAY_L, value & 0xFF);
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HDELAY_H, (value >> 8) & 0xFF);

	value = mode->hsync_end - mode->hsync_start;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HDURATION_L, value & 0xFF);
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_HDURATION_H, (value >> 8) & 0xFF);

	value = mode->vtotal;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_VTOTAL_L, value & 0xFF);
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_VTOTAL_H, (value >> 8) & 0xFF);

	value = mode->vtotal - mode->vdisplay;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_VBLANK, value & 0xFF);

	value = mode->vtotal - mode->vsync_start;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_VDELAY, value & 0xFF);

	value = mode->vsync_end - mode->vsync_start;
	hdmi_writeb(hdmi, HDMI_VIDEO_EXT_VDURATION, value & 0xFF);

	hdmi_writeb(hdmi, HDMI_PHY_PRE_DIV_RATIO, 0x1e);
	hdmi_writeb(hdmi, HDMI_PHY_FEEDBACK_DIV_RATIO_LOW, 0x2c);
	hdmi_writeb(hdmi, HDMI_PHY_FEEDBACK_DIV_RATIO_HIGH, 0x01);

	return 0;
}

static int inno_hdmi_setup(struct inno_hdmi *hdmi,
			   struct drm_display_mode *mode)
{
	struct drm_display_info *display = &hdmi->connector.display_info;

	hdmi->hdmi_data.vic = drm_match_cea_mode(mode);

	hdmi->hdmi_data.enc_in_format = HDMI_COLORSPACE_RGB;
	hdmi->hdmi_data.enc_out_format = HDMI_COLORSPACE_RGB;

	if ((hdmi->hdmi_data.vic == 6) || (hdmi->hdmi_data.vic == 7) ||
	    (hdmi->hdmi_data.vic == 21) || (hdmi->hdmi_data.vic == 22) ||
	    (hdmi->hdmi_data.vic == 2) || (hdmi->hdmi_data.vic == 3) ||
	    (hdmi->hdmi_data.vic == 17) || (hdmi->hdmi_data.vic == 18))
		hdmi->hdmi_data.colorimetry = HDMI_COLORIMETRY_ITU_601;
	else
		hdmi->hdmi_data.colorimetry = HDMI_COLORIMETRY_ITU_709;

	/* Mute video and audio output */
	hdmi_modb(hdmi, HDMI_AV_MUTE, m_AUDIO_MUTE | m_VIDEO_BLACK,
		  v_AUDIO_MUTE(1) | v_VIDEO_MUTE(1));

	/* Set HDMI Mode */
	hdmi_writeb(hdmi, HDMI_HDCP_CTRL,
		    v_HDMI_DVI(display->is_hdmi));

	inno_hdmi_config_video_timing(hdmi, mode);

	inno_hdmi_config_video_csc(hdmi);

	if (display->is_hdmi) {
		inno_hdmi_config_video_avi(hdmi, mode);
		inno_hdmi_config_video_vsi(hdmi, mode);
	}

	/*
	 * When IP controller have configured to an accurate video
	 * timing, then the TMDS clock source would be switched to
	 * DCLK_LCDC, so we need to init the TMDS rate to mode pixel
	 * clock rate, and reconfigure the DDC clock.
	 */
	hdmi->tmds_rate = mode->clock * 1000;
	inno_hdmi_i2c_init(hdmi);

	/* Unmute video and audio output */
	hdmi_modb(hdmi, HDMI_AV_MUTE, m_VIDEO_BLACK, v_VIDEO_MUTE(0));
	if (hdmi->audio_enable)
		hdmi_modb(hdmi, HDMI_AV_MUTE, m_AUDIO_MUTE, v_AUDIO_MUTE(0));

	return 0;
}

static void inno_hdmi_encoder_mode_set(struct drm_encoder *encoder,
				       struct drm_display_mode *mode,
				       struct drm_display_mode *adj_mode)
{
	struct inno_hdmi *hdmi = to_inno_hdmi(encoder);

	inno_hdmi_setup(hdmi, adj_mode);

	/* Store the display mode for plugin/DPMS poweron events */
	drm_mode_copy(&hdmi->previous_mode, adj_mode);
}

static void inno_hdmi_encoder_enable(struct drm_encoder *encoder)
{
	struct inno_hdmi *hdmi = to_inno_hdmi(encoder);

	inno_hdmi_set_pwr_mode(hdmi, NORMAL);
}

static void inno_hdmi_encoder_disable(struct drm_encoder *encoder)
{
	struct inno_hdmi *hdmi = to_inno_hdmi(encoder);

	inno_hdmi_set_pwr_mode(hdmi, LOWER_PWR);
}

static bool inno_hdmi_encoder_mode_fixup(struct drm_encoder *encoder,
					 const struct drm_display_mode *mode,
					 struct drm_display_mode *adj_mode)
{
	return true;
}

static int
inno_hdmi_encoder_atomic_check(struct drm_encoder *encoder,
			       struct drm_crtc_state *crtc_state,
			       struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);

	s->output_mode = ROCKCHIP_OUT_MODE_P888;
	s->output_type = DRM_MODE_CONNECTOR_HDMIA;

	return 0;
}

static struct drm_encoder_helper_funcs inno_hdmi_encoder_helper_funcs = {
	.enable     = inno_hdmi_encoder_enable,
	.disable    = inno_hdmi_encoder_disable,
	.mode_fixup = inno_hdmi_encoder_mode_fixup,
	.mode_set   = inno_hdmi_encoder_mode_set,
	.atomic_check = inno_hdmi_encoder_atomic_check,
};

static enum drm_connector_status
inno_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	struct inno_hdmi *hdmi = connector_to_inno_hdmi(connector);

	return (hdmi_readb(hdmi, HDMI_STATUS) & m_HOTPLUG) ?
		connector_status_connected : connector_status_disconnected;
}

static int inno_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct inno_hdmi *hdmi = connector_to_inno_hdmi(connector);
	struct edid *edid;
	int ret = 0;

	if (!hdmi->ddc)
		return 0;

	edid = drm_get_edid(connector, hdmi->ddc);
	if (edid) {
		hdmi->hdmi_data.sink_has_audio = drm_detect_monitor_audio(edid);
		drm_connector_update_edid_property(connector, edid);
		ret = drm_add_edid_modes(connector, edid);
		kfree(edid);
	}

	return ret;
}

static enum drm_mode_status
inno_hdmi_connector_mode_valid(struct drm_connector *connector,
			       struct drm_display_mode *mode)
{
	return MODE_OK;
}

static int
inno_hdmi_probe_single_connector_modes(struct drm_connector *connector,
				       uint32_t maxX, uint32_t maxY)
{
	return drm_helper_probe_single_connector_modes(connector, 1920, 1080);
}

static void inno_hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs inno_hdmi_connector_funcs = {
	.fill_modes = inno_hdmi_probe_single_connector_modes,
	.detect = inno_hdmi_connector_detect,
	.destroy = inno_hdmi_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct drm_connector_helper_funcs inno_hdmi_connector_helper_funcs = {
	.get_modes = inno_hdmi_connector_get_modes,
	.mode_valid = inno_hdmi_connector_mode_valid,
};

static int
inno_hdmi_audio_config_set(struct inno_hdmi *hdmi,
			   struct hdmi_codec_daifmt *daifmt,
			   struct audio_info *audio)
{
	int rate, N, channel;

	if (audio->channels < 3)
		channel = I2S_CHANNEL_1_2;
	else if (audio->channels < 5)
		channel = I2S_CHANNEL_3_4;
	else if (audio->channels < 7)
		channel = I2S_CHANNEL_5_6;
	else
		channel = I2S_CHANNEL_7_8;

	switch (audio->sample_rate) {
	case 32000:
		rate = AUDIO_32K;
		N = N_32K;
		break;
	case 44100:
		rate = AUDIO_441K;
		N = N_441K;
		break;
	case 48000:
		rate = AUDIO_48K;
		N = N_48K;
		break;
	case 88200:
		rate = AUDIO_882K;
		N = N_882K;
		break;
	case 96000:
		rate = AUDIO_96K;
		N = N_96K;
		break;
	case 176400:
		rate = AUDIO_1764K;
		N = N_1764K;
		break;
	case 192000:
		rate = AUDIO_192K;
		N = N_192K;
		break;
	default:
		dev_err(hdmi->dev, "[%s] not support such sample rate %d\n",
			__func__, audio->sample_rate);
		return -ENOENT;
	}

	if (daifmt->fmt == HDMI_SPDIF) {
		/* set_audio source SPDIF */
		hdmi_writeb(hdmi, HDMI_AUDIO_CTRL1, 0x09);
	} else {
		/* set_audio source I2S */
		hdmi_writeb(hdmi, HDMI_AUDIO_CTRL1, 0x01);
	}
	hdmi_writeb(hdmi, AUDIO_SAMPLE_RATE, rate);
	hdmi_writeb(hdmi, AUDIO_I2S_MODE, v_I2S_MODE(I2S_STANDARD) |
		    v_I2S_CHANNEL(channel));

	hdmi_writeb(hdmi, AUDIO_I2S_MAP, 0x00);
	hdmi_writeb(hdmi, AUDIO_I2S_SWAPS_SPDIF, rate);

	/* Set N value */
	hdmi_writeb(hdmi, AUDIO_N_H, (N >> 16) & 0x0F);
	hdmi_writeb(hdmi, AUDIO_N_M, (N >> 8) & 0xFF);
	hdmi_writeb(hdmi, AUDIO_N_L, N & 0xFF);

	/* Set hdmi nlpcm mode to support hdmi bitstream */
	hdmi_writeb(hdmi, HDMI_AUDIO_CHANNEL_STATUS, v_AUDIO_STATUS_NLPCM(0));

	return inno_hdmi_config_audio_aai(hdmi, audio);
}

static int inno_hdmi_audio_prepare(struct device *dev, void *data,
				   struct hdmi_codec_daifmt *fmt,
				   struct hdmi_codec_params *hparms)
{
	struct inno_hdmi *hdmi = dev_get_drvdata(dev);

	if (!hdmi->hdmi_data.sink_has_audio) {
		dev_err(hdmi->dev, "Sink do not support audio!\n");
		return -ENODEV;
	}

	hdmi->audio_enable = 0;
	hdmi_modb(hdmi, HDMI_AV_MUTE, m_AUDIO_PD, v_AUDIO_PD(1));
	return 0;
}

static int inno_hdmi_audio_hw_params(struct device *dev, void *d,
				     struct hdmi_codec_daifmt *daifmt,
				     struct hdmi_codec_params *params)
{
	struct inno_hdmi *hdmi = dev_get_drvdata(dev);
	struct audio_info audio = {
		.sample_width = params->sample_width,
		.sample_rate = params->sample_rate,
		.channels = params->channels,
	};

	if (!hdmi->hdmi_data.sink_has_audio) {
		dev_err(hdmi->dev, "Sink do not support audio!\n");
		return -ENODEV;
	}

	if (!hdmi->encoder.crtc)
		return -ENODEV;

	switch (daifmt->fmt) {
	case HDMI_I2S:
		break;
	case HDMI_SPDIF:
		break;
	default:
		dev_err(dev, "%s: Invalid format %d\n", __func__, daifmt->fmt);
		return -EINVAL;
	}

	return inno_hdmi_audio_config_set(hdmi, daifmt, &audio);
}

static void inno_hdmi_audio_shutdown(struct device *dev, void *d)
{
	/* do nothing */
}

static int inno_hdmi_audio_mute(struct device *dev, void *data, bool mute, int direction)
{
	struct inno_hdmi *hdmi = dev_get_drvdata(dev);

	if (!hdmi->hdmi_data.sink_has_audio) {
		dev_err(hdmi->dev, "Sink do not support audio!\n");
		return -ENODEV;
	}

	hdmi->audio_enable = !mute;

	if (mute)
		hdmi_modb(hdmi, HDMI_AV_MUTE, m_AUDIO_MUTE | m_AUDIO_PD,
			  v_AUDIO_MUTE(1) | v_AUDIO_PD(1));
	else
		hdmi_modb(hdmi, HDMI_AV_MUTE, m_AUDIO_MUTE | m_AUDIO_PD,
			  v_AUDIO_MUTE(0) | v_AUDIO_PD(0));

	return 0;
}

static int inno_hdmi_audio_get_eld(struct device *dev, void *d,
				   uint8_t *buf, size_t len)
{
	struct inno_hdmi *hdmi = dev_get_drvdata(dev);
	struct drm_mode_config *config = &hdmi->encoder.dev->mode_config;
	struct drm_connector *connector;
	int ret = -ENODEV;

	mutex_lock(&config->mutex);
	list_for_each_entry(connector, &config->connector_list, head) {
		if (&hdmi->encoder == connector->encoder) {
			memcpy(buf, connector->eld,
			       min(sizeof(connector->eld), len));
			ret = 0;
		}
	}
	mutex_unlock(&config->mutex);

	return ret;
}

static const struct hdmi_codec_ops audio_codec_ops = {
	.hw_params = inno_hdmi_audio_hw_params,
	.prepare = inno_hdmi_audio_prepare,
	.audio_shutdown = inno_hdmi_audio_shutdown,
	.mute_stream = inno_hdmi_audio_mute,
	.get_eld = inno_hdmi_audio_get_eld,
};

static int inno_hdmi_audio_codec_init(struct inno_hdmi *hdmi,
				      struct device *dev)
{
	const char *str = "i2s";
	struct hdmi_codec_pdata codec_data = {
		.i2s = 1,
		.spdif = 0,
		.ops = &audio_codec_ops,
		.max_i2s_channels = 8,
	};

	if (device_property_read_string(dev, "rockchip,format", &str))
		dev_warn(dev, "can not get rockchip,format\n");

	if (strstr(str, "spdif")) {
		codec_data.i2s = 0;
		codec_data.spdif = 1;
	}

	hdmi->audio_enable = false;
	hdmi->audio_pdev = platform_device_register_data(
				dev, HDMI_CODEC_DRV_NAME, PLATFORM_DEVID_NONE,
				&codec_data, sizeof(codec_data));

	return PTR_ERR_OR_ZERO(hdmi->audio_pdev);
}

static int inno_hdmi_register(struct drm_device *drm, struct inno_hdmi *hdmi)
{
	struct drm_encoder *encoder = &hdmi->encoder;
	struct device *dev = hdmi->dev;

	encoder->possible_crtcs = rockchip_drm_of_find_possible_crtcs(drm, dev->of_node);

	/*
	 * If we failed to find the CRTC(s) which this encoder is
	 * supposed to be connected to, it's because the CRTC has
	 * not been registered yet.  Defer probing, and hope that
	 * the required CRTC is added later.
	 */
	if (encoder->possible_crtcs == 0)
		return -EPROBE_DEFER;

	drm_encoder_helper_add(encoder, &inno_hdmi_encoder_helper_funcs);
	drm_simple_encoder_init(drm, encoder, DRM_MODE_ENCODER_TMDS);

	hdmi->connector.polled = DRM_CONNECTOR_POLL_HPD;

	drm_connector_helper_add(&hdmi->connector,
				 &inno_hdmi_connector_helper_funcs);
	drm_connector_init_with_ddc(drm, &hdmi->connector,
				    &inno_hdmi_connector_funcs,
				    DRM_MODE_CONNECTOR_HDMIA,
				    hdmi->ddc);

	drm_connector_attach_encoder(&hdmi->connector, encoder);
	inno_hdmi_audio_codec_init(hdmi, dev);

	return 0;
}

static irqreturn_t inno_hdmi_i2c_irq(struct inno_hdmi *hdmi)
{
	struct inno_hdmi_i2c *i2c = hdmi->i2c;
	u8 stat;

	stat = hdmi_readb(hdmi, HDMI_INTERRUPT_STATUS1);
	if (!(stat & m_INT_EDID_READY))
		return IRQ_NONE;

	/* Clear HDMI EDID interrupt flag */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);

	complete(&i2c->cmp);

	return IRQ_HANDLED;
}

static irqreturn_t inno_hdmi_hardirq(int irq, void *dev_id)
{
	struct inno_hdmi *hdmi = dev_id;
	irqreturn_t ret = IRQ_NONE;
	u8 interrupt;

	if (hdmi->i2c)
		ret = inno_hdmi_i2c_irq(hdmi);

	interrupt = hdmi_readb(hdmi, HDMI_STATUS);
	if (interrupt & m_INT_HOTPLUG) {
		hdmi_modb(hdmi, HDMI_STATUS, m_INT_HOTPLUG, m_INT_HOTPLUG);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t inno_hdmi_irq(int irq, void *dev_id)
{
	struct inno_hdmi *hdmi = dev_id;

	drm_helper_hpd_irq_event(hdmi->connector.dev);

	return IRQ_HANDLED;
}

static int inno_hdmi_i2c_read(struct inno_hdmi *hdmi, struct i2c_msg *msgs)
{
	int length = msgs->len;
	u8 *buf = msgs->buf;
	int ret;

	ret = wait_for_completion_timeout(&hdmi->i2c->cmp, HZ / 10);
	if (!ret)
		return -EAGAIN;

	while (length--)
		*buf++ = hdmi_readb(hdmi, HDMI_EDID_FIFO_ADDR);

	return 0;
}

static int inno_hdmi_i2c_write(struct inno_hdmi *hdmi, struct i2c_msg *msgs)
{
	/*
	 * The DDC module only support read EDID message, so
	 * we assume that each word write to this i2c adapter
	 * should be the offset of EDID word address.
	 */
	if ((msgs->len != 1) ||
	    ((msgs->addr != DDC_ADDR) && (msgs->addr != DDC_SEGMENT_ADDR)))
		return -EINVAL;

	reinit_completion(&hdmi->i2c->cmp);

	if (msgs->addr == DDC_SEGMENT_ADDR)
		hdmi->i2c->segment_addr = msgs->buf[0];
	if (msgs->addr == DDC_ADDR)
		hdmi->i2c->ddc_addr = msgs->buf[0];

	/* Set edid fifo first addr */
	hdmi_writeb(hdmi, HDMI_EDID_FIFO_OFFSET, 0x00);

	/* Set edid word address 0x00/0x80 */
	hdmi_writeb(hdmi, HDMI_EDID_WORD_ADDR, hdmi->i2c->ddc_addr);

	/* Set edid segment pointer */
	hdmi_writeb(hdmi, HDMI_EDID_SEGMENT_POINTER, hdmi->i2c->segment_addr);

	return 0;
}

static int inno_hdmi_i2c_xfer(struct i2c_adapter *adap,
			      struct i2c_msg *msgs, int num)
{
	struct inno_hdmi *hdmi = i2c_get_adapdata(adap);
	struct inno_hdmi_i2c *i2c = hdmi->i2c;
	int i, ret = 0;

	mutex_lock(&i2c->lock);

	/* Clear the EDID interrupt flag and unmute the interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, m_INT_EDID_READY);
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);

	for (i = 0; i < num; i++) {
		DRM_DEV_DEBUG(hdmi->dev,
			      "xfer: num: %d/%d, len: %d, flags: %#x\n",
			      i + 1, num, msgs[i].len, msgs[i].flags);

		if (msgs[i].flags & I2C_M_RD)
			ret = inno_hdmi_i2c_read(hdmi, &msgs[i]);
		else
			ret = inno_hdmi_i2c_write(hdmi, &msgs[i]);

		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

	/* Mute HDMI EDID interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, 0);

	mutex_unlock(&i2c->lock);

	return ret;
}

static u32 inno_hdmi_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm inno_hdmi_algorithm = {
	.master_xfer	= inno_hdmi_i2c_xfer,
	.functionality	= inno_hdmi_i2c_func,
};

static struct i2c_adapter *inno_hdmi_i2c_adapter(struct inno_hdmi *hdmi)
{
	struct i2c_adapter *adap;
	struct inno_hdmi_i2c *i2c;
	int ret;

	i2c = devm_kzalloc(hdmi->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return ERR_PTR(-ENOMEM);

	mutex_init(&i2c->lock);
	init_completion(&i2c->cmp);

	adap = &i2c->adap;
	adap->class = I2C_CLASS_DDC;
	adap->owner = THIS_MODULE;
	adap->dev.parent = hdmi->dev;
	adap->dev.of_node = hdmi->dev->of_node;
	adap->algo = &inno_hdmi_algorithm;
	strlcpy(adap->name, "Inno HDMI", sizeof(adap->name));
	i2c_set_adapdata(adap, hdmi);

	ret = i2c_add_adapter(adap);
	if (ret) {
		dev_warn(hdmi->dev, "cannot add %s I2C adapter\n", adap->name);
		devm_kfree(hdmi->dev, i2c);
		return ERR_PTR(ret);
	}

	hdmi->i2c = i2c;

	DRM_DEV_INFO(hdmi->dev, "registered %s I2C bus driver\n", adap->name);

	return adap;
}

static struct inno_hdmi_phy_config rk3036_hdmi_phy_config[] = {
	/* pixelclk pre-emp vlev */
	{ 74250000,  0x3f, 0xbb },
	{ 165000000, 0x6f, 0xbb },
	{ ~0UL,	     0x00, 0x00 }
};

static struct inno_hdmi_phy_config rk3128_hdmi_phy_config[] = {
	/* pixelclk pre-emp vlev */
	{ 74250000,  0x3f, 0xaa },
	{ 165000000, 0x5f, 0xaa },
	{ ~0UL,	     0x00, 0x00 }
};

static const struct inno_hdmi_plat_data rk3036_hdmi_drv_data = {
	.dev_type   = RK3036_HDMI,
	.phy_config = rk3036_hdmi_phy_config,
};

static const struct inno_hdmi_plat_data rk3128_hdmi_drv_data = {
	.dev_type   = RK3128_HDMI,
	.phy_config = rk3128_hdmi_phy_config,
};

static const struct of_device_id inno_hdmi_dt_ids[] = {
	{ .compatible = "rockchip,rk3036-inno-hdmi",
	  .data = &rk3036_hdmi_drv_data,
	},
	{ .compatible = "rockchip,rk3128-inno-hdmi",
	  .data = &rk3128_hdmi_drv_data,
	},
	{},
};
MODULE_DEVICE_TABLE(of, inno_hdmi_dt_ids);

static int inno_hdmi_bind(struct device *dev, struct device *master,
				 void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = data;
	struct inno_hdmi *hdmi;
	int irq;
	int ret;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->dev = dev;
	hdmi->drm_dev = drm;
	hdmi->plat_data = device_get_match_data(hdmi->dev);

	hdmi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hdmi->regs))
		return PTR_ERR(hdmi->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	hdmi->aclk = devm_clk_get(hdmi->dev, "aclk");
	if (IS_ERR(hdmi->aclk)) {
		dev_err(hdmi->dev, "Unable to get HDMI aclk clk\n");
		return PTR_ERR(hdmi->aclk);
	}

	hdmi->pclk = devm_clk_get(hdmi->dev, "pclk");
	if (IS_ERR(hdmi->pclk)) {
		DRM_DEV_ERROR(hdmi->dev, "Unable to get HDMI pclk clk\n");
		return PTR_ERR(hdmi->pclk);
	}

	ret = clk_prepare_enable(hdmi->aclk);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev,
			      "Cannot enable HDMI aclk clock: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(hdmi->pclk);
	if (ret) {
		dev_err(hdmi->dev, "Cannot enable HDMI pclk clock: %d\n", ret);
		goto err_disable_aclk;
	}

	inno_hdmi_reset(hdmi);

	hdmi->ddc = inno_hdmi_i2c_adapter(hdmi);
	if (IS_ERR(hdmi->ddc)) {
		ret = PTR_ERR(hdmi->ddc);
		hdmi->ddc = NULL;
		goto err_disable_pclk;
	}

	/*
	 * When IP controller haven't configured to an accurate video
	 * timing, then the TMDS clock source would be switched to
	 * PCLK_HDMI, so we need to init the TMDS rate to PCLK rate,
	 * and reconfigure the DDC clock.
	 */
	hdmi->tmds_rate = clk_get_rate(hdmi->pclk);
	inno_hdmi_i2c_init(hdmi);

	ret = inno_hdmi_register(drm, hdmi);
	if (ret)
		goto err_put_adapter;

	dev_set_drvdata(dev, hdmi);

	/* Unmute hotplug interrupt */
	hdmi_modb(hdmi, HDMI_STATUS, m_MASK_INT_HOTPLUG, v_MASK_INT_HOTPLUG(1));

	ret = devm_request_threaded_irq(dev, irq, inno_hdmi_hardirq,
					inno_hdmi_irq, IRQF_SHARED,
					dev_name(dev), hdmi);
	if (ret) {
		dev_err(hdmi->dev,
			"failed to request hdmi irq: %d\n", ret);
		goto err_cleanup_hdmi;
	}

	return 0;

err_cleanup_hdmi:
	hdmi->connector.funcs->destroy(&hdmi->connector);
	hdmi->encoder.funcs->destroy(&hdmi->encoder);
err_put_adapter:
	i2c_put_adapter(hdmi->ddc);
err_disable_pclk:
	clk_disable_unprepare(hdmi->pclk);
err_disable_aclk:
	clk_disable_unprepare(hdmi->aclk);
	return ret;
}

static void inno_hdmi_unbind(struct device *dev, struct device *master,
			     void *data)
{
	struct inno_hdmi *hdmi = dev_get_drvdata(dev);

	hdmi->connector.funcs->destroy(&hdmi->connector);
	hdmi->encoder.funcs->destroy(&hdmi->encoder);

	i2c_put_adapter(hdmi->ddc);
	clk_disable_unprepare(hdmi->pclk);
	clk_disable_unprepare(hdmi->aclk);
}

static const struct component_ops inno_hdmi_ops = {
	.bind	= inno_hdmi_bind,
	.unbind	= inno_hdmi_unbind,
};

static int inno_hdmi_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &inno_hdmi_ops);
}

static int inno_hdmi_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &inno_hdmi_ops);

	return 0;
}

struct platform_driver inno_hdmi_driver = {
	.probe  = inno_hdmi_probe,
	.remove = inno_hdmi_remove,
	.driver = {
		.name = "innohdmi-rockchip",
		.of_match_table = inno_hdmi_dt_ids,
	},
};
