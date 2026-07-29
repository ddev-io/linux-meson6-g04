/*
 * Goodix GT82X touchscreen driver.
 *
 * The controller protocol and configuration layout are based on the Goodix
 * vendor driver. Coordinates are reported as supplied by the controller; the
 * board configuration must provide the correct sensor orientation.
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>

#define GT82X_TOUCH_ADDR	0x0f40
#define GT82X_PRE_CMD_H		0x0f
#define GT82X_PRE_CMD_L		0xff
#define GT82X_END_CMD_H		0x80
#define GT82X_END_CMD_L		0x00
#define GT82X_MAX_TOUCHES	5
#define GT82X_CONFIG_LENGTH	114
#define GT82X_REPORT_LENGTH	(2 + GT82X_MAX_TOUCHES * 5 + 1)

struct gt82x_ts {
	struct i2c_client *client;
	struct input_dev *input;
	int interrupt_gpio;
	int reset_gpio;
	int power_gpio;
};

/*
 * Working G04/VB100a Pro channel map for the 1280x800 panel. The first two
 * bytes are the GT82X configuration address. No software coordinate swap or
 * polarity transform is used.
 */
static const u8 gt82x_g04_config[GT82X_CONFIG_LENGTH] = {
	0x0f, 0x80, 0x1c, 0x0d, 0x1b, 0x0c, 0x1a, 0x0b,
	0x19, 0x0a, 0x18, 0x09, 0x17, 0x08, 0x16, 0x07,
	0x15, 0x06, 0x14, 0x05, 0x13, 0x04, 0x12, 0x03,
	0x11, 0x02, 0x10, 0x01, 0x0f, 0x00, 0xff, 0x1d,
	0x13, 0x09, 0x12, 0x08, 0x11, 0x07, 0x10, 0x06,
	0x0f, 0x05, 0x0e, 0x04, 0x0d, 0x03, 0x0c, 0x02,
	0xff, 0x01, 0x0b, 0x00, 0x0b, 0x03, 0x88, 0x00,
	0x00, 0x19, 0x00, 0x00, 0x03, 0x00, 0x00, 0x0e,
	0x50, 0x3c, 0x15, 0x03, 0x00, 0x05, 0x00, 0x05,
	0x00, 0x03, 0x20, 0x1b, 0x1a, 0x2e, 0x2c, 0x08,
	0x00, 0x03, 0x19, 0x05, 0x14, 0x10, 0x00, 0x07,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01,
};

static int gt82x_i2c_write(struct i2c_client *client, const u8 *buf,
			   size_t len)
{
	int ret = i2c_master_send(client, buf, len);

	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static int gt82x_i2c_read(struct i2c_client *client, u16 reg, u8 *buf,
			  size_t len)
{
	u8 addr[] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.len = sizeof(addr),
			.buf = addr,
		}, {
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static int gt82x_send_end_cmd(struct gt82x_ts *ts)
{
	const u8 command[] = { GT82X_END_CMD_H, GT82X_END_CMD_L };

	return gt82x_i2c_write(ts->client, command, sizeof(command));
}

static int gt82x_initialize_panel(struct gt82x_ts *ts)
{
	int ret;

	ret = gt82x_i2c_write(ts->client, gt82x_g04_config,
			      sizeof(gt82x_g04_config));
	if (ret)
		return ret;

	ret = gt82x_send_end_cmd(ts);
	if (ret)
		return ret;

	msleep(500);
	return 0;
}

static int gt82x_request_gpio(struct device *dev, const char *name,
			      unsigned long flags)
{
	int gpio = of_get_named_gpio(dev->of_node, name, 0);
	int ret;

	if (gpio == -EPROBE_DEFER)
		return gpio;
	if (!gpio_is_valid(gpio))
		return gpio < 0 ? gpio : -EINVAL;

	ret = devm_gpio_request_one(dev, gpio, flags, name);
	if (ret)
		return ret;

	return gpio;
}

static int gt82x_hw_reset(struct gt82x_ts *ts)
{
	gpio_set_value(ts->power_gpio, 1);
	msleep(10);
	gpio_set_value(ts->reset_gpio, 0);
	msleep(50);
	gpio_set_value(ts->reset_gpio, 1);
	msleep(50);

	return 0;
}

static void gt82x_report(struct gt82x_ts *ts, const u8 *data)
{
	u8 fingers = data[0] & 0x1f;
	u8 checksum = 0;
	const u8 *p = data + 2;
	int i;

	if ((data[0] & 0xc0) != 0x80)
		return;

	for (i = 0; i < GT82X_MAX_TOUCHES; i++) {
		bool active = fingers & BIT(i);

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);
		if (active) {
			u16 x = (p[0] << 8) | p[1];
			u16 y = (p[2] << 8) | p[3];

			input_report_abs(ts->input, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, 15);
			checksum += p[0] + p[1] + p[2] + p[3] + p[4];
			p += 5;
		}
	}

	if (checksum != *p) {
		dev_warn_ratelimited(&ts->client->dev,
				     "invalid touch report checksum\n");
		return;
	}

	input_mt_sync_frame(ts->input);
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);
}

static irqreturn_t gt82x_irq_thread(int irq, void *dev_id)
{
	struct gt82x_ts *ts = dev_id;
	u8 data[GT82X_REPORT_LENGTH];
	int ret;

	ret = gt82x_i2c_read(ts->client, GT82X_TOUCH_ADDR, data, sizeof(data));
	gt82x_send_end_cmd(ts);
	if (ret) {
		dev_err_ratelimited(&ts->client->dev,
				    "failed to read touch report: %d\n", ret);
		return IRQ_HANDLED;
	}

	gt82x_report(ts, data);
	return IRQ_HANDLED;
}

static int gt82x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct gt82x_ts *ts;
	struct input_dev *input;
	const u8 pre_cmd[] = { GT82X_PRE_CMD_H, GT82X_PRE_CMD_L };
	int ret;

	if (!client->dev.of_node)
		return -ENODEV;
	if (!client->irq)
		return -EINVAL;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EIO;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->interrupt_gpio = gt82x_request_gpio(&client->dev,
					       "interrupt-gpios", GPIOF_IN);
	if (ts->interrupt_gpio < 0)
		return ts->interrupt_gpio;
	ts->power_gpio = gt82x_request_gpio(&client->dev, "power-gpios",
					   GPIOF_OUT_INIT_HIGH);
	if (ts->power_gpio < 0)
		return ts->power_gpio;
	ts->reset_gpio = gt82x_request_gpio(&client->dev, "reset-gpios",
					   GPIOF_OUT_INIT_HIGH);
	if (ts->reset_gpio < 0)
		return ts->reset_gpio;

	gt82x_hw_reset(ts);

	ret = gt82x_i2c_write(client, pre_cmd, sizeof(pre_cmd));
	if (ret) {
		dev_err(&client->dev, "I2C pre-command failed: %d\n", ret);
		return ret;
	}

	ret = gt82x_initialize_panel(ts);
	if (ret) {
		dev_err(&client->dev, "panel configuration failed: %d\n", ret);
		return ret;
	}

	input = devm_input_allocate_device(&client->dev);
	if (!input)
		return -ENOMEM;

	ts->input = input;
	input->name = "Goodix GT82X Touchscreen";
	input->phys = "input/ts";
	input->id.bustype = BUS_I2C;
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 1280, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 800, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	ret = input_mt_init_slots(input, GT82X_MAX_TOUCHES,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	i2c_set_clientdata(client, ts);
	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					gt82x_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					dev_name(&client->dev), ts);
	if (ret) {
		dev_err(&client->dev, "failed to request IRQ: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "G04 GT82X initialized at 1280x800\n");
	return 0;
}

static const struct i2c_device_id gt82x_id[] = {
	{ "gt82x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gt82x_id);

static const struct of_device_id gt82x_of_match[] = {
	{ .compatible = "goodix,gt82x-g04" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt82x_of_match);

static struct i2c_driver gt82x_driver = {
	.probe = gt82x_probe,
	.id_table = gt82x_id,
	.driver = {
		.name = "gt82x",
		.of_match_table = gt82x_of_match,
	},
};
module_i2c_driver(gt82x_driver);

MODULE_AUTHOR("ddev-io");
MODULE_DESCRIPTION("Goodix GT82X touchscreen driver for G04");
MODULE_LICENSE("GPL v2");
