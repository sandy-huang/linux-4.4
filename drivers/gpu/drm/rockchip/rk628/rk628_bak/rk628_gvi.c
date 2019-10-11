// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017 Rockchip Electronics Co. Ltd.
 *
 * Author: Sandy Huang <hjc@rock-chips.com>
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/mfd/rk628.h>

#include <drm/drmP.h>
#include <drm/drm_of.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_panel.h>

#include <video/of_display_timing.h>
#include <video/videomode.h>


enum {
	gvi_FORMAT_VESA_24BIT,
	gvi_FORMAT_JEIDA_24BIT,
	gvi_FORMAT_JEIDA_18BIT,
	gvi_FORMAT_VESA_18BIT,
};

struct rk628_gvi {
	struct drm_bridge base;
	struct drm_connector connector;
	struct drm_panel *panel;
	struct device *dev;
	struct regmap *regmap;
	struct clk *clock;
	struct rk628 *parent;
	bool division_mode;
	u32 bus_format;
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
	struct drm_display_info *info = &connector->display_info;
	u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	int num_modes = 0;

	num_modes = drm_panel_get_modes(gvi->panel);

	if (info->num_bus_formats)
		gvi->bus_format = info->bus_formats[0];
	else
		gvi->bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG;

	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	return num_modes;
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
#if 0
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);
	u8 format;
	u32 value;

	clk_prepare_enable(gvi->clock);

	rk628_frc_dclk_invert(gvi->parent);
	rk628_frc_dither_init(gvi->parent, gvi->bus_format);

	switch (gvi->bus_format) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_JEIDA:
		format = gvi_FORMAT_JEIDA_18BIT;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		format = gvi_FORMAT_JEIDA_24BIT;
		break;
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		format = gvi_FORMAT_VESA_18BIT;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
	default:
		format = gvi_FORMAT_VESA_24BIT;
		break;
	}

	value = gvi_CON_CHA0TTL_DISABLE | gvi_CON_CHA1TTL_DISABLE |
		gvi_CON_CHA0_POWER_UP | gvi_CON_CBG_POWER_UP |
		gvi_CON_PLL_POWER_UP | gvi_CON_SELECT(format);

	if (gvi->dual_channel)
		value |= gvi_CON_CHA1_POWER_UP | gvi_DCLK_INV |
			 gvi_CON_CHASEL_DOUBLE_CHANNEL;
	else
		value |= gvi_CON_CHA1_POWER_DOWN |
			 gvi_CON_CHASEL_SINGLE_CHANNEL;

	regmap_write(gvi->regmap, rk628_gvi_CON, value);

	drm_panel_prepare(gvi->panel);
	drm_panel_enable(gvi->panel);
#endif
}

static void rk628_gvi_bridge_disable(struct drm_bridge *bridge)
{
	struct rk628_gvi *gvi = bridge_to_gvi(bridge);

	drm_panel_disable(gvi->panel);
	drm_panel_unprepare(gvi->panel);

	//regmap_write(gvi->regmap, rk628_gvi_CON,
	//	     gvi_CON_CHA0_POWER_DOWN | gvi_CON_CHA1_POWER_DOWN |
	//	     gvi_CON_CBG_POWER_DOWN | gvi_CON_PLL_POWER_DOWN);

	//clk_disable_unprepare(gvi->clock);
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
				 DRM_MODE_CONNECTOR_GVI);
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

static int rk628_gvi_probe(struct platform_device *pdev)
{
	struct rk628 *rk628 = dev_get_drvdata(pdev->dev.parent);
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
	gvi->parent = rk628;
	platform_set_drvdata(pdev, gvi);

	ret = rk628_gvi_parse_dt(gvi);
	if (ret) {
		dev_err(dev, "failed to parse DT\n");
		return ret;
	}

	gvi->regmap = dev_get_regmap(dev->parent, NULL);
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
		//return ret;
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

MODULE_AUTHOR("Sandy Huang<hjc@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip RK628 GVI driver");
MODULE_LICENSE("GPL v2");
