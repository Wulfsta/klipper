from . import lis2dw

def load_config(config):
    return lis2dw.LIS2DW(config, lis2dw.LIS3DH_TYPE)

def load_config_prefix(config):
    return lis2dw.LIS2DW(config, lis2dw.LIS3DH_TYPE)
