################################################################################
#
# brcmfmac-ap6275p
#
################################################################################

BRCMFMAC_AP6275P_VERSION = 75259e6b655cf8e08a0e6f893a4f8ea28dd17271
BRCMFMAC_AP6275P_SITE = https://github.com/armbian/firmware/raw/$(BRCMFMAC_AP6275P_VERSION)/brcm
BRCMFMAC_AP6275P_SOURCE =
BRCMFMAC_AP6275P_LICENSE = Broadcom proprietary

# Individual firmware files to download
BRCMFMAC_AP6275P_FILES = \
	brcmfmac43752-pcie.bin \
	brcmfmac43752-pcie.clm_blob \
	brcmfmac43752-pcie.txt

define BRCMFMAC_AP6275P_EXTRACT_CMDS
	# Nothing to extract — files downloaded directly
endef

define BRCMFMAC_AP6275P_BUILD_CMDS
	# Nothing to build
endef

define BRCMFMAC_AP6275P_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/lib/firmware/brcm
	for f in $(BRCMFMAC_AP6275P_FILES); do \
		if [ -f $(BRCMFMAC_AP6275P_DL_DIR)/$$f ]; then \
			$(INSTALL) -m 0644 $(BRCMFMAC_AP6275P_DL_DIR)/$$f \
				$(TARGET_DIR)/lib/firmware/brcm/$$f; \
		fi; \
	done
	# Create symlink for board-specific NVRAM config
	cd $(TARGET_DIR)/lib/firmware/brcm && \
		ln -sf brcmfmac43752-pcie.txt brcmfmac43752-pcie.AP6275P.txt
endef

# Download each file individually
define BRCMFMAC_AP6275P_DOWNLOAD_CMDS
	$(INSTALL) -d $(BRCMFMAC_AP6275P_DL_DIR)
	for f in $(BRCMFMAC_AP6275P_FILES); do \
		$(call DOWNLOAD,$(BRCMFMAC_AP6275P_SITE)/$$f,$(BRCMFMAC_AP6275P_DL_DIR)/$$f); \
	done
endef

$(eval $(generic-package))
