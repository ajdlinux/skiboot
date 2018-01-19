.. _device-tree/opencapi:

=============================
OpenCAPI Device Tree Bindings
=============================

NPU bindings:

.. code-block:: dts

  xscom@603fc00000000 {
    npu@5011000 {
      compatible = "ibm,power9-npu";
      phandle = <0xe6>;
      ibm,phb-index = <0x7>;
      reg = <0x5011000 0x2c>;
      ibm,npu-index = <0x0>;
      ibm,npu-links = <0x2>; /* Number of links wired up to this npu. */

      link@2 {
        compatible = "ibm,npu-link-opencapi";
        ibm,npu-group-id = <0x1>;
	ibm,npu-lane-mask = <0xf1e000>; /* Mask specifying which IBM PHY lanes
	                                 * are used for this link. 24-bit,
	                                 * lane 0 is most significant bit */
        ibm,npu-phy = <0x80000000 0x9010c3f>; /* SCOM address of the IBM PHY
	                                       * controlling this link. */
	ibm,npu-link-index = <0x2>; /* Hardware link index.
                                     * Used to calculate various address offsets. */
	phandle = <0xe7>;
      };

      link@3 {
        compatible = "ibm,npu-link-opencapi";
	ibm,npu-group-id = <0x2>;
	ibm,npu-lane-mask = <0x78f>;
	ibm,npu-phy = <0x80000000 0x9010c3f>;
	ibm,npu-link-index = <0x3>;
	phandle = <0xe8>;
      };
    };
  };

PCI device bindings
-------------------

.. code-block:: dts

  pciex@600e800000000 {
    compatible = "ibm,power9-npu-opencapi-pciex", "ibm,ioda2-npu2-opencapi-phb";
    ibm,mmio-window = <0x600e8 0x0 0x8 0x0>;
    ibm,opal-num-pes = <0x10>;
    device_type = "pciex";
    ibm,links = <0x1>;
    ibm,phb-diag-data-size = <0x0>;
    ibm,xscom-base = <0x5011000>;
    ranges = <0x2000000 0x600e8 0x0 0x600e8 0x0 0x8 0x0>;
    #interrupt-cells = <0x1>;
    bus-range = <0x0 0xff>;
    interrupt-parent = <0xdf>;
    #address-cells = <0x3>;
    ibm,opal-phbid = <0x0 0x5>;
    ibm,npcq = <0xe6>; /* phandle to the NPU node */
    ibm,chip-id = <0x0>;
    #size-cells = <0x2>;
    phandle = <0x581>;
    reg = <0x600e8 0x0 0x8 0x0>;
    clock-frequency = <0x200 0x0>;
    ibm,npu-index = <0x0>;

    device@0 {
      ibm,opal-xsl-irq = <0x58>;
      revision-id = <0x0>;
      ibm,opal-xsl-mmio = <0x60302 0x1d0000 0x60302 0x1d0008 0x60302 0x1d0010 0x60302 0x1d0018>;
      ibm,pci-config-space-type = <0x1>;
      class-code = <0x120000>;
      vendor-id = <0x1014>;
      device-id = <0x62b>;
      phandle = <0x58d>;
      reg = <0x0 0x0 0x0 0x0 0x0>;
    };

    device@0,1 {
      ibm,opal-xsl-irq = <0x58>;
      revision-id = <0x0>;
      ibm,opal-xsl-mmio = <0x60302 0x1d0000 0x60302 0x1d0008 0x60302 0x1d0010 0x60302 0x1d0018>;
      ibm,pci-config-space-type = <0x1>;
      class-code = <0x120000>;
      vendor-id = <0x1014>;
      device-id = <0x62b>;
      phandle = <0x58e>;
      reg = <0x100 0x0 0x0 0x0 0x0>;
    };
  };

