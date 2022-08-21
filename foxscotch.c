#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>

#define IR35217_MODULE_NAME		"foxscotch"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Wolf9466");
MODULE_DESCRIPTION("A simple IR35217 test driver.");
MODULE_VERSION("0.69");

static struct i2c_device_id ir35217_idtable[] =
{
	{ "IR35217", 0 },
	{ "ir35217", 0 },
	NULL
};

MODULE_ALIAS(IR35217_MODULE_NAME);

// Our context for an IR35217
struct ir35217_device_data
{
	struct i2c_client *client;
	struct device *hwmon_dev;
	
	// Step size for Loop 1 & 2 VIDs in mV.
	// Should be 5mV or 10mV.
	uint8_t L1VIDStep, L2VIDStep;
	// Phases - for IR35217, there are eight
	// total, and loop 2 maxes out at 2 phases.
	uint8_t L1Phases, L2Phases;
	// Phase multiplier. 0 = None; 1 = Doubled
	// 2 = None; 3 = Quadrupled.
	uint8_t L1PhaseMul, L2PhaseMul;
};


// It's gonna be here on AMD GPUs - if you got one elsewhere,
// you might wanna add the address in here.
static unsigned short normal_i2c[] = {
	0x30, 0x32, I2C_CLIENT_END
};

// Takes the loop configuration data and returns the number
// of phases present for loop 1.
static uint8_t Loop1CfgValue[21] =
{
	8, 7, 6, 5, 4, 3, 2, 1, 6, 5, 4, 3, 2, 1, 6, 5, 4, 3, 2, 1, 7
};

// Ditto for loop 2.
static uint8_t Loop2CfgValue[21] =
{
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 1
};

// I2C plumbing.
static int AMDI2CReadByte(struct i2c_client *client, uint8_t reg, uint8_t *OutByte)
{
	struct i2c_msg Msgs[2];
	
	memset(Msgs, 0x00, sizeof(struct i2c_msg) * 2);
	
	Msgs[0].addr = client->addr;
	Msgs[0].flags = 0;
	Msgs[0].len = 1;
	Msgs[0].buf = &reg;

	Msgs[1].addr = client->addr;
	Msgs[1].flags = I2C_M_RD | I2C_M_STOP;
	Msgs[1].len = 1;
	Msgs[1].buf = OutByte;
	
	if(!client->adapter->algo->master_xfer)
		return 0;
	else
		return(client->adapter->algo->master_xfer(client->adapter, Msgs, 2));
}

static int AMDI2CWriteByte(struct i2c_client *client, uint8_t reg, uint8_t data)
{
	struct i2c_msg WrMsg;
	uint8_t InBytes[2];
		
	InBytes[0] = reg;
	InBytes[1] = data;
	
	memset(&WrMsg, 0x00, sizeof(struct i2c_msg));

	WrMsg.addr = client->addr;
	WrMsg.flags = I2C_M_STOP;
	WrMsg.len = 2;
	WrMsg.buf = InBytes;

	if(!client->adapter->algo->master_xfer)
		return 0;
	else
		return(client->adapter->algo->master_xfer(client->adapter, &WrMsg, 1));
}



// pec_mode == 0: support both PEC and non-PEC; 1: Support only non-PEC
// 2: support only PEC
ssize_t pec_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	struct i2c_client *client = drvdata->client;
	uint8_t OutByte = 0; 

	if(!i2c_verify_client(&client->dev))
		printk(KERN_INFO "Client fucked UwU\n");
	
	// Fetch register 0x4D; shift off low 6 bits
	AMDI2CReadByte(client, 0x4D, &OutByte);
	OutByte >>= 6;
	
	return(sprintf(buf, "%d\n", OutByte));
}

ssize_t loops_configuration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	return(sprintf(buf, "%d+%d\n", drvdata->L1Phases, drvdata->L2Phases));
}

ssize_t loop1_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	struct i2c_client *client = drvdata->client;
	int8_t OffsetVID = 0;				// Remember, this is signed on purpose
	int Result = 0;
	
	if(!i2c_verify_client(&client->dev))
	{
		printk(KERN_INFO "Client fucked UwU\n");
		return(0);
	}
	
	// Fetch register 0xE1
	AMDI2CReadByte(client, 0xE1, &OffsetVID);
	
	// Convert from VID to mV, preserving sign
	Result = OffsetVID * drvdata->L1VIDStep;
	
	return(sprintf(buf, "%d\n", Result));
}

ssize_t loop2_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	struct i2c_client *client = drvdata->client;
	int8_t OffsetVID = 0;
	int Result = 0;
	
	if(!i2c_verify_client(&client->dev))
	{
		printk(KERN_INFO "Client fucked UwU\n");
		return(0);
	}
	
	// Fetch register 0xE1
	AMDI2CReadByte(client, 0xE2, &OffsetVID);
	
	// Convert from VID to mV, preserving sign
	Result = (OffsetVID * 6250) / 1000;
	
	return(sprintf(buf, "%d\n", Result));
}

// Takes input in mV specified in decimal. To specify a negative
// offset, simply prefix number with '-'.
ssize_t loop1_offset_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	struct i2c_client *client = drvdata->client;
	int8_t VID;
	long int arg;
	
	if(kstrtol(buf, 10, &arg))
	{
		printk(KERN_INFO "WARNING: Unable to parse input \"%s\" as an offset.\n", buf);
		return -EINVAL;
	}
		
	if(!drvdata->L1VIDStep)
	{
		printk(KERN_INFO "WARNING: Unable to set offset; VID step size unknown.\n");
		return 0;
	}
	
	// Sanity check the input - allow +/-500mV max.
	if((arg > 500) || (arg < -500))
	{
		printk(KERN_INFO "WARNING: Out-of-range offset %ld specified. Ignoring.\n", arg);
		return -EINVAL;
	}
	
	printk(KERN_INFO "Got %ld as argument.\n", arg);
	
	VID = (arg * 1000) / 6250;
	
	printk(KERN_INFO "Would write VID 0x%02X\n", VID);
	
	// Write register 0xE1
	AMDI2CWriteByte(client, 0xE1, VID);

	return count;
}

// Takes input in mV specified in decimal. To specify a negative
// offset, simply prefix number with '-'.
ssize_t loop2_offset_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct ir35217_device_data *drvdata = dev_get_drvdata(dev);
	struct i2c_client *client = drvdata->client;
	int8_t VID;
	long int arg;

	if(kstrtol(buf, 10, &arg))
	{
		printk(KERN_INFO "WARNING: Unable to parse input \"%s\" as an offset.\n", buf);
		return -EINVAL;
	}
		
	if(!drvdata->L2VIDStep)
	{
		printk(KERN_INFO "WARNING: Unable to set offset; VID step size unknown.\n");
		return 0;
	}
	
	// Sanity check the input - allow +/-500mV max.
	if((arg > 500) || (arg < -500))
	{
		printk(KERN_INFO "WARNING: Out-of-range offset %ld specified. Ignoring.\n", arg);
		return -EINVAL;
	}
	
	VID = arg / drvdata->L2VIDStep;
	
	// Write register 0xE2
	AMDI2CWriteByte(client, 0xE2, VID);

	return count;
}

DEVICE_ATTR_RO(loops_configuration);
DEVICE_ATTR_RW(loop1_offset);
DEVICE_ATTR_RW(loop2_offset);

struct attribute *ir35217_attrs[] = {
		&dev_attr_loops_configuration.attr,
		&dev_attr_loop1_offset.attr,
		&dev_attr_loop2_offset.attr,
		NULL
};

ATTRIBUTE_GROUPS(ir35217);

/*
int ir35217_probe(struct i2c_client *client)
{
	printk(KERN_INFO "YEEETUS PROBED!\n");
	
	// drvdata is now our i2c client
	dev_set_drvdata(&client->dev, client);

	devm_hwmon_device_register_with_groups(&client->dev, client->name, client, ir35217_groups);
	return(0);
}*/

static umode_t ir35217_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	switch(type)
	{
		case hwmon_temp:
			return 0444;
		case hwmon_in:
			return 0444;
		case hwmon_curr:
			return 0444;
		default:
			return 0;
	}
}

static uint16_t ir35217_get_temp(struct ir35217_device_data *drvdata, int channel)
{
	struct i2c_client *client = drvdata->client;
	uint8_t OutByte;
		
	// Sanity check
	if(channel <  0 || channel > 1) return(0);
	
	// Value 0x08 == temp of loop 1 in 0xB4
	// Value 0x09 == temp of loop 2 in 0xB4
	AMDI2CWriteByte(client, 0xE5, 0x08 + channel);
	AMDI2CReadByte(client, 0xB4, &OutByte);

	return(OutByte * 1000);
}

static uint32_t ir35217_get_vout(struct ir35217_device_data *drvdata, int channel)
{
	struct i2c_client *client = drvdata->client;
	uint32_t Result;
	uint8_t OutByte;
	
	// Sanity check
	if(channel < 0 || channel > 1) return(0);
	
	// Value 0x04 == voltage of loop 1 in 0xB4
	// Value 0x05 == voltage of loop 2 in 0xB4
	AMDI2CWriteByte(client, 0xE5, 0x04 + channel);
	AMDI2CReadByte(client, 0xB4, &OutByte);
	printk(KERN_INFO "Raw vout result was 0x%08X UwU\n", OutByte);
	Result = OutByte * 15625UL; // 15.625f is the multiplier for mV, so we'll do one better
	Result /= 1000;				// Now chop off the low digits
	return(Result);
}


static uint32_t ir35217_get_iout(struct ir35217_device_data *drvdata, int channel)
{
	struct i2c_client *client = drvdata->client;
	uint32_t Result;
	uint8_t OutByte;
	
	// Sanity check
	if(channel < 0 || channel > 1) return(0);
	
	// Value 0x06 == current of loop 1 in 0xB4
	// Value 0x07 == current of loop 2 in 0xB4
	AMDI2CWriteByte(client, 0xE5, 0x06 + channel);
	AMDI2CReadByte(client, 0xB4, &OutByte);
	printk(KERN_DEBUG "Raw iout result was 0x%08X UwU\n", OutByte);

	// loop 2 reports in 0.5A units
	Result = OutByte * ((channel == 1) ? 2000 : 1000);
	return(Result);
}

// The critical temp best fits the IR35217 with the temp_max register.
// This is less than the emergency temp, which would be ovtp_thresh, as
// at that point the VR shuts down.
static uint32_t ir35217_get_crit_temp(struct ir35217_device_data *drvdata, int channel)
{
	struct i2c_client *client = drvdata->client;
	uint8_t OutByte;
	
	AMDI2CReadByte(client, 0x48, &OutByte);
	
	// Isolate bits [7:2]
	OutByte >>= 2;
	
	// Base value is 64; 1 deg C resolution.
	return((64 + OutByte) * 1000);
}

static int ir35217_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	struct ir35217_device_data *devdata = dev_get_drvdata(dev);
	
	switch(type)
	{
		case hwmon_temp:
			if(attr == hwmon_temp_input)
				*val = ir35217_get_temp(devdata, channel);
			else if(attr == hwmon_temp_crit)
				*val = ir35217_get_crit_temp(devdata, channel);
			else
				return -EINVAL;
			
			break;
		case hwmon_in:
			if(attr == hwmon_in_input)
				*val = ir35217_get_vout(devdata, channel);
			else
				return -EINVAL;
			break;
		case hwmon_curr:
			if(attr == hwmon_curr_input)
				*val = ir35217_get_iout(devdata, channel);
			else
				return -EINVAL;
			break;
		default:
			return -EINVAL;
	}
	
	return 0;
}

static const char *ir35217_temp_label[] = {
	"Loop 1",
	"Loop 2",
};

static const char *ir35217_in_label[] = {
	"Loop 1",
	"Loop 2",
};

static const char *ir35217_curr_label[] = {
	"Loop 1",
	"Loop 2",
};

static int ir35217_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, const char **str)
{
	if(channel < 0 || channel > 1) return -EINVAL;
	
	switch(type)
	{
		case hwmon_temp:
			*str = ir35217_temp_label[channel];
			break;
		case hwmon_in:
			*str = ir35217_in_label[channel];
			break;
		case hwmon_curr:
			*str = ir35217_curr_label[channel];
			break;
		default:
			return -EINVAL;
	}
	
	return 0;
}

static const struct hwmon_ops ir35217_hwmon_ops =
{
	.is_visible = ir35217_is_visible,
	.read = ir35217_read,
	.read_string = ir35217_read_string,
};

static const struct hwmon_channel_info *ir35217_info[] =
{
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_CRIT, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info ir35217_chip_info =
{
	.ops = &ir35217_hwmon_ops,
	.info = ir35217_info,
};

#define IR35217_PHASE_MUL_DOUBLED					0x01
#define IR35217_PHASE_MUL_QUADRUPLED				0x03

// IR35217 identification registers
// Should be 'I' 'R', for ID regs 1 and 2, respectively.
#define INFINEON_SALEM_I2C_ID_REG_1                 0xFC
#define INFINEON_SALEM_I2C_ID_REG_2                 0xFD

// Model register
#define INFINEON_SALEM_I2C_MODEL_ID_REG             0xFB

// For use with the model register
#define INFINEON_SALEM_I2C_MODEL_IR35217			0x5F

int ir35217_probe(struct i2c_client *client)
{
	uint8_t res;
	struct ir35217_device_data *devdata = devm_kzalloc(&client->dev, sizeof(struct ir35217_device_data), GFP_KERNEL);
	
	if(!devdata) return -ENOMEM;
	
	// Lowest bit is VID step for loop 2, next bit
	// up is VID step for loop 1. Units are mV.
	AMDI2CReadByte(client, 0x58, &res);
	devdata->L1VIDStep = (!(res & 2))? 10 : 5;
	devdata->L2VIDStep = (!(res & 1)) ? 10 : 5;
	
	// Fetch register 0x27; isolate low 5 bits
	AMDI2CReadByte(client, 0x27, &res);
	res &= 0x1F;

	if(res > 20)
	{
		printk(KERN_INFO "Loop configuration value does not look valid.\n");
		devdata->L1Phases = devdata->L2Phases = 0;
	}
	else
	{
		devdata->L1Phases = Loop1CfgValue[res];
		devdata->L2Phases = Loop2CfgValue[res];
	}
	
	// Read 0x44, extract highest two bits
	AMDI2CReadByte(client, 0x44, &res);
	res >>= 6;
	
	devdata->L1PhaseMul = res;
	
	// Ditto for 0x6A
	AMDI2CReadByte(client, 0x6A, &res);
	res >>= 6;
	
	devdata->L2PhaseMul = res;
	
	printk(KERN_INFO "Probed IR35217 (uwu) at address 0x%02X!\n", client->addr);
	
	// Dump Loop 1 info
	if(devdata->L1PhaseMul == IR35217_PHASE_MUL_DOUBLED)
		printk(KERN_INFO "Loop 1 has %d phases (doubled)\n", devdata->L1Phases);
	else if(devdata->L1PhaseMul == IR35217_PHASE_MUL_QUADRUPLED)
		printk(KERN_INFO "Loop 1 has %d phases (quadrupled)\n", devdata->L1Phases);
	else
		printk(KERN_INFO "Loop 1 has %d phases.\n", devdata->L1Phases);
	
	// Dump loop 2 info
	if(devdata->L2PhaseMul == IR35217_PHASE_MUL_DOUBLED)
		printk(KERN_INFO "Loop 2 has %d phases (doubled)\n", devdata->L2Phases);
	else if(devdata->L2PhaseMul == IR35217_PHASE_MUL_QUADRUPLED)
		printk(KERN_INFO "Loop 2 has %d phases (quadrupled)\n", devdata->L2Phases);
	else
		printk(KERN_INFO "Loop 2 has %d phases.\n", devdata->L2Phases);
	
	devdata->client = client;
	
	// devdata is now our drvdata
	dev_set_drvdata(&client->dev, devdata);
	
	devdata->hwmon_dev = devm_hwmon_device_register_with_info(&client->dev, client->name, devdata, &ir35217_chip_info, ir35217_groups);
	return(0);
}

int ir35217_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	uint8_t byte0, byte1;
	
	AMDI2CReadByte(client, INFINEON_SALEM_I2C_ID_REG_1, &byte0);
	AMDI2CReadByte(client, INFINEON_SALEM_I2C_ID_REG_2, &byte1);

	if(byte0 != 'I' && byte1 != 'R')
	{
		printk(KERN_DEBUG "Device at 0x%04X is not an Infineon Salem device.\n", client->addr);
		return(-ENODEV);
	}

	AMDI2CReadByte(client, INFINEON_SALEM_I2C_MODEL_ID_REG, &byte0);
	if(byte0 != INFINEON_SALEM_I2C_MODEL_IR35217)
	{
		printk(KERN_DEBUG "Device at 0x%04X might be an Infineon device, but it's not an IR35217.\n", client->addr);
		return(-ENODEV);
	}
	
	// It is definitely an IR35217; make sure it's in AMD mode
	AMDI2CReadByte(client, 0x27, &byte0);
	
	if(!((byte0 >> 5) & 1))
	{
		printk(KERN_INFO "Found an IR35217 at 0x%04X, but it is not in AMD mode! Skipping.\n", client->addr);
		return(-ENODEV);
	}

	strlcpy(info->type, "IR35217", I2C_NAME_SIZE);
	return(0);
}

struct i2c_driver ir35217_driver =
{
	.class = I2C_CLASS_HWMON,
	.driver = { .name = "IR35217" },
	.id_table = ir35217_idtable,
	.probe_new = ir35217_probe,
	.detect = ir35217_detect,
	.address_list = normal_i2c,
};

static int __init foxscotch_init(void)
{
	i2c_add_driver(&ir35217_driver);
 	printk(KERN_INFO "Foxscotch IR35217 driver loaded.\n");

	return(0);
}
static void __exit foxscotch_exit(void)
{
	i2c_del_driver(&ir35217_driver);
	printk(KERN_INFO "Foxscotch IR35217 driver unloading.\n");
}

module_init(foxscotch_init);
module_exit(foxscotch_exit);
