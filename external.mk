# Include Orange Pi 5 Plus hardware acceleration packages
include $(sort $(wildcard $(NERVES_DEFCONFIG_DIR)/package/*/*.mk))
