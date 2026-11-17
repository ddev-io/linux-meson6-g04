obj-$(CONFIG_ARCH_MESON) += meson.o

obj-$(CONFIG_MACH_MESON6) += g04-power.o

obj-$(CONFIG_MACH_MESON6) += g04-sdio-clock.o

ifeq ($(CONFIG_MACH_MESON6),y)
obj-$(CONFIG_SMP) += g04-smp.o
endif
