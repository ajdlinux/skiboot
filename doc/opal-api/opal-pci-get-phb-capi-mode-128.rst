OPAL_PCI_GET_PHB_CAPI_MODE
==========================

Get a PHB's current CAPI mode status.

Parameter
---------

``uint64_t phb_id``
  is the value from the PHB node ibm,opal-phbid property.

Return Codes
------------
OPAL_PARAMETER
  The specified PHB was not found.

OPAL_UNSUPPORTED
  The specified PHB does not support this operation.

OPAL_PHB_CAPI_MODE_CAPI
  The PHB is currently in CAPI mode.

OPAL_PHB_CAPI_MODE_PCIE
  The PHB is currently in regular PCIe mode.
