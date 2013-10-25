A simple keyboard event mapper that doesn't depend on anything but
udev/evdev.

To be able to run this as user:

    # setcap CAP_DAC_READ_SEARCH=ep evdevd
