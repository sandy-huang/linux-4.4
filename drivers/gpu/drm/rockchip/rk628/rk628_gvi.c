// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017 Rockchip Electronics Co. Ltd.
 *
 * Author: Wyon Bi <bivvy.bi@rock-chips.com>
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/mfd/rk618.h>

#include <drm/drmP.h>
#include <drm/drm_of.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_panel.h>

#include <video/of_display_timing.h>
#include <video/videomode.h>

#define HOSTREG(x)		((x) + 0x0)//todo
#define GVI_SYS_CTRL0		HOSTREG(0x0000)
#define GVI_SYS_CTRL1		HOSTREG(0x0004)
#define GVI_SYS_CTRL2		HOSTREG(0x0008)
#define GVI_SYS_CTRL3		HOSTREG(0x000c)
#define GVI_VERSION		HOSTREG(0x0010)
#define GVI_SYS_RST		HOSTREG(0x0014)
#define GVI_LINE_FLAG		HOSTREG(0x0018)
#define GVI_STATUS		HOSTREG(0x001c)
#define GVI_PHY_CTRL0		HOSTREG(0x0020)
#define GVI_PHY_CTRL1		HOSTREG(0x0024)
#define GVI_PHY_CTRL2		HOSTREG(0x0028)
#define GVI_PLL_LOCK_TIMEOUT	HOSTREG(0x0030)
#define GVI_HTPDN_TIMEOUT	HOSTREG(0x0034)
#define GVI_LOCKN_TIMEOUT	HOSTREG(0x0038)
#define GVI_WAIT_LOCKN		HOSTREG(0x003C)
#define GVI_WAIT_HTPDN		HOSTREG(0x0040)
#define GVI_INTR_EN		HOSTREG(0x0050)
#define GVI_INTR_CLR		HOSTREG(0x0054)
#define GVI_INTR_RAW_STATUS	HOSTREG(0x0058)
#define GVI_INTR_STATUS		HOSTREG(0x005c)

struct rk628_gvi {
	struct drm_bridge base;
	struct drm_connector connector;
	struct drm_panel *panel;
	struct device *dev;
	struct regmap *regmap;
	struct clk *clock;
	struct rk618 *parent;
	bool division_mode;
	u8 lane_num;
	u8 byte_mode;
	u32 format;
};

static inline struct rk628_gvi *bridge_to_gvi(struct drm_bridge *b)
{
	return container_of(b, struct rk628_gvi, base);
}

static inline struct rk628_gvi *connector_to_gvi(struct drm_connector *c)
{
	return container_of(c, struct rk628_gvi, connector);
}

static struct drm_encoder *
rk628_gvi_connector_best_encoder(struct drm_connector *connector)
{
	struct rk628_gvi *gvi = connector_to_gvi(connector);

	return gvi->base.encoder;
}

static int rk628_gvi_connector_get_modes(struct drm_connector *connector)
{
	struct rk628_gvi *gvi = connector_to_gvi(connector);

	return drm_panel_get_modes(gvi->panel);
}

static const struct drm_connector_helper_funcs
rk628_gvi_connector_helper_funcs = {
	.get_modes = rk628_gvi_connector_get_modes,
	.best_encoder = rk628_gvi_connector_best_encoder,
};

static enum drm_connector_status
rk628_gvi_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void rk628_gvi_connector_destroy(struct drm_connector *connector)
{
	struct rk628_gvi *gvi = connector_to_gvi(connector);

	drm_panel_detach(gvi->panel);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs rk628_gvi_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.detect = rk628_gvi_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = rk628_gvi_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void rk628_gvi_bridge_enable(struct drm_bridge *bridge)
{
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);
	u32 value;

	clk_prepare_enable(gvi->clock);

	value = LVDS_CON_CHA0TTL_DISABLE | LVDS_CON_CHA1TTL_DISABLE |
		LVDS_CON_CHA0_POWER_UP | LVDS_CON_CBG_POWER_UP |
		LVDS_CON_PLL_POWER_UP | LVDS_CON_SELECT(gvi->format);

	if (gvi->division_mode)
		value |= LVDS_CON_CHA1_POWER_UP | LVDS_DCLK_INV |
			 LVDS_CON_CHASEL_DOUBLE_CHANNEL;
	else
		value |= LVDS_CON_CHA1_POWER_DOWN |
			 LVDS_CON_CHASEL_SINGLE_CHANNEL;

	regmap_write(gvi->regmap, RK618_LVDS_CON, value);

	drm_panel_prepare(gvi->panel);
	drm_panel_enable(gvi->panel);
}

static void rk628_gvi_bridge_disable(struct drm_bridge *bridge)
{
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);

	drm_panel_disable(gvi->panel);
	drm_panel_unprepare(gvi->panel);

	regmap_write(gvi->regmap, RK618_LVDS_CON,
		     LVDS_CON_CHA0_POWER_DOWN | LVDS_CON_CHA1_POWER_DOWN |
		     LVDS_CON_CBG_POWER_DOWN | LVDS_CON_PLL_POWER_DOWN);

	clk_disable_unprepare(gvi->clock);
}

static void rk628_gvi_bridge_mode_set(struct drm_bridge *bridge,
				       struct drm_display_mode *mode,
				       struct drm_display_mode *adj)
{
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);
	struct drm_connector *connector = &gvi->connector;
	struct drm_display_info *info = &connector->display_info;
	u32 bus_format;

	if (info->num_bus_formats)
		bus_format = info->bus_formats[0];
	else
		bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG;

	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:	/* jeida-18 */
		gvi->format = 0;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:	/* jeida-24 */
		gvi->format = 0;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:	/* vesa-24 */
		gvi->format = 0;
		break;
	default:
		gvi->format = 0;
		break;
	}
}

static int rk628_gvi_bridge_attach(struct drm_bridge *bridge)
{
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);
	struct device *dev = gvi->dev;
	struct drm_connector *connector = &gvi->connector;
	struct drm_device *drm = bridge->dev;
	int ret;

	connector->port = dev->of_node;

	ret = drm_connector_init(drm, connector, &rk628_gvi_connector_funcs,
				 DRM_MODE_CONNECTOR_LVDS);
	if (ret) {
		dev_err(gvi->dev, "Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_helper_add(connector, &rk628_gvi_connector_helper_funcs);
	drm_mode_connector_attach_encoder(connector, bridge->encoder);

	ret = drm_panel_attach(gvi->panel, connector);
	if (ret) {
		dev_err(gvi->dev, "Failed to attach panel\n");
		return ret;
	}

	return 0;
}

static const struct drm_bridge_funcs rk628_gvi_bridge_funcs = {
	.attach = rk628_gvi_bridge_attach,
	.mode_set = rk628_gvi_bridge_mode_set,
	.enable = rk628_gvi_bridge_enable,
	.disable = rk628_gvi_bridge_disable,
};

static int rk628_gvi_parse_dt(struct rk628_gvi *gvi)
{
	struct device *dev = gvi->dev;

	gvi->division_mode = of_property_read_bool(dev->of_node,
						   "division-mode");

	return 0;
}

static const struct regmap_config rk628_gvi_regmap_config = {
	.name = "gvi",
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = GVI_INTR_STATUS,
	.reg_format_endian = REGMAP_ENDIAN_NATIVE,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
};

static int rk628_gvi_probe(struct platform_device *pdev)
{
	struct rk618 *rk618 = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct device_node *endpoint;
	struct rk628_gvi *gvi;
	int ret;

	if (!of_device_is_available(dev->of_node))
		return -ENODEV;

	gvi = devm_kzalloc(dev, sizeof(*gvi), GFP_KERNEL);
	if (!gvi)
		return -ENOMEM;

	gvi->dev = dev;
	gvi->parent = rk618;
	platform_set_drvdata(pdev, gvi);

	ret = rk628_gvi_parse_dt(gvi);
	if (ret) {
		dev_err(dev, "failed to parse DT\n");
		return ret;
	}

	gvi->regmap = devm_regmap_init_i2c(rk618->client,
					   &rk628_gvi_regmap_config);
	if (!gvi->regmap)
		return -ENODEV;

	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 1, -1);
	if (endpoint) {
		struct device_node *remote;

		remote = of_graph_get_remote_port_parent(endpoint);
		of_node_put(endpoint);
		if (!remote) {
			dev_err(dev, "no panel connected\n");
			return -ENODEV;
		}

		gvi->panel = of_drm_find_panel(remote);
		of_node_put(remote);
		if (!gvi->panel) {
			dev_err(dev, "Waiting for panel driver\n");
			return -EPROBE_DEFER;
		}
	}

	gvi->clock = devm_clk_get(dev, "gvi");
	if (IS_ERR(gvi->clock)) {
		ret = PTR_ERR(gvi->clock);
		dev_err(dev, "failed to get gvi clock: %d\n", ret);
		return ret;
	}

	gvi->base.funcs = &rk628_gvi_bridge_funcs;
	gvi->base.of_node = dev->of_node;
	ret = drm_bridge_add(&gvi->base);
	if (ret) {
		dev_err(dev, "failed to add drm_bridge: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rk628_gvi_remove(struct platform_device *pdev)
{
	struct rk628_gvi *gvi = platform_get_drvdata(pdev);

	drm_bridge_remove(&gvi->base);

	return 0;
}

static const struct of_device_id rk628_gvi_of_match[] = {
	{ .compatible = "rockchip,rk628-gvi", },
	{},
};
MODULE_DEVICE_TABLE(of, rk628_gvi_of_match);

static struct platform_driver rk628_gvi_driver = {
	.driver = {
		.name = "rk628-gvi",
		.of_match_table = of_match_ptr(rk628_gvi_of_match),
	},
	.probe = rk628_gvi_probe,
	.remove = rk628_gvi_remove,
};
module_platform_driver(rk628_gvi_driver);

MODULE_AUTHOR("Wyon Bi <bivvy.bi@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip RK628 GVI driver");
MODULE_LICENSE("GPL v2");
