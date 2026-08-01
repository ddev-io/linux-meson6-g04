#
# aml_nftl
#

obj-$(CONFIG_MTD_NAND_MESON6_AML_NFTL)	+= aml_nftl.o
aml_nftl-objs =
aml_nftl-objs += aml_nftl_block.o
aml_nftl-objs += aml_nftl_core.o
aml_nftl-objs += aml_nftl_wl.o
aml_nftl-objs += aml_nftl_ops.o

# The inspection build keeps legacy mutation helpers unreachable by design.
ccflags-y += -Wno-error=unused-function
