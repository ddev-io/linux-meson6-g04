/* SPDX-License-Identifier: GPL-2.0-only */
/* Read battery telemetry from the G04's AXP20x, retaining firmware settings. */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

#define G04_AXP_INPUT_STATUS	0x00
#define G04_AXP_CHARGE_STATUS	0x01
#define G04_AXP_CHIP_ID		0x03
#define G04_AXP_VBAT		0x78
#define G04_AXP_CAPACITY	0xb9
#define G04_AXP_OCV		0xbc
#define G04_AXP_CHARGING	BIT(6)
#define G04_AXP_BAT_PRESENT	BIT(5)
#define G04_AXP_INPUT_VALID	(BIT(6) | BIT(4))

struct g04_battery {
	struct i2c_client *client;
	struct power_supply supply;
};

static enum power_supply_property g04_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static int g04_battery_read_uv(struct i2c_client *client, u8 reg)
{
	int high, low;

	high = i2c_smbus_read_byte_data(client, reg);
	if (high < 0)
		return high;
	low = i2c_smbus_read_byte_data(client, reg + 1);
	if (low < 0)
		return low;
	return ((high << 4) | (low & 0xf)) * 1100;
}

static int g04_battery_capacity(struct i2c_client *client, bool charging)
{
	static const struct {
		u16 mv;
		u8 charging;
		u8 discharging;
	} curve[] = {
		{ 3555,   0,   0 }, { 3625,  1,   6 }, { 3660,  2,  11 },
		{ 3696,   6,  19 }, { 3731, 12,  31 }, { 3766, 18,  42 },
		{ 3801,  27,  52 }, { 3836, 44,  58 }, { 3872, 52,  64 },
		{ 3942,  67,  74 }, { 4012, 78,  84 }, { 4083, 88,  92 },
		{ 4153, 100, 100 },
	};
	int cap, uv, mv;
	unsigned int i;

	cap = i2c_smbus_read_byte_data(client, G04_AXP_CAPACITY);
	if (cap < 0)
		return cap;
	if (!(cap & BIT(7)) && cap <= 100)
		return cap;

	/* Use the vendor G04 open-circuit voltage curve if the gauge is off. */
	uv = g04_battery_read_uv(client, G04_AXP_OCV);
	if (uv < 0)
		return uv;
	if (uv < 2500000 || uv > 4500000)
		return -ENODATA;
	mv = uv / 1000;
	if (mv <= curve[0].mv)
		return 0;
	for (i = 1; i < ARRAY_SIZE(curve); i++) {
		int lo, hi;

		if (mv > curve[i].mv)
			continue;
		lo = charging ? curve[i - 1].charging : curve[i - 1].discharging;
		hi = charging ? curve[i].charging : curve[i].discharging;
		return lo + (hi - lo) * (mv - curve[i - 1].mv) /
			    (curve[i].mv - curve[i - 1].mv);
	}
	return 100;
}

static int g04_battery_get(struct power_supply *supply,
			   enum power_supply_property prop,
			   union power_supply_propval *val)
{
	struct g04_battery *bat = container_of(supply, struct g04_battery, supply);
	struct i2c_client *client = bat->client;
	int status, value;

	status = i2c_smbus_read_byte_data(client, G04_AXP_CHARGE_STATUS);
	if (status < 0)
		return status;

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		value = !!(status & G04_AXP_BAT_PRESENT);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!(status & G04_AXP_BAT_PRESENT)) {
			value = POWER_SUPPLY_STATUS_UNKNOWN;
		} else if (status & G04_AXP_CHARGING) {
			value = POWER_SUPPLY_STATUS_CHARGING;
		} else {
			int input = i2c_smbus_read_byte_data(client, G04_AXP_INPUT_STATUS);

			if (input < 0)
				return input;
			value = input & G04_AXP_INPUT_VALID ?
				POWER_SUPPLY_STATUS_NOT_CHARGING :
				POWER_SUPPLY_STATUS_DISCHARGING;
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!(status & G04_AXP_BAT_PRESENT))
			return -ENODATA;
		value = g04_battery_capacity(client, status & G04_AXP_CHARGING);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (!(status & G04_AXP_BAT_PRESENT))
			return -ENODATA;
		value = g04_battery_read_uv(client, G04_AXP_VBAT);
		break;
	default:
		return -EINVAL;
	}

	if (value < 0)
		return value;
	val->intval = value;
	return 0;
}

static int g04_battery_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct g04_battery *bat;
	int chip;

	chip = i2c_smbus_read_byte_data(client, G04_AXP_CHIP_ID);
	if (chip < 0)
		return chip;
	if (chip == 0xff || !chip)
		return -ENODEV;

	bat = devm_kzalloc(&client->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;
	bat->client = client;
	bat->supply.name = "battery";
	bat->supply.type = POWER_SUPPLY_TYPE_BATTERY;
	bat->supply.properties = g04_battery_properties;
	bat->supply.num_properties = ARRAY_SIZE(g04_battery_properties);
	bat->supply.get_property = g04_battery_get;
	i2c_set_clientdata(client, bat);
	return power_supply_register(&client->dev, &bat->supply);
}

static int g04_battery_remove(struct i2c_client *client)
{
	struct g04_battery *bat = i2c_get_clientdata(client);

	power_supply_unregister(&bat->supply);
	return 0;
}

static const struct i2c_device_id g04_battery_ids[] = {
	{ "meson6-g04-battery", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, g04_battery_ids);

static const struct of_device_id g04_battery_of_match[] = {
	{ .compatible = "ddev-io,meson6-g04-axp20-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, g04_battery_of_match);

static struct i2c_driver g04_battery_driver = {
	.probe = g04_battery_probe,
	.remove = g04_battery_remove,
	.id_table = g04_battery_ids,
	.driver = {
		.name = "meson6-g04-battery",
		.of_match_table = g04_battery_of_match,
	},
};
module_i2c_driver(g04_battery_driver);
MODULE_LICENSE("GPL");
