#!/system/bin/sh

# EliteKernel: deploy modules and misc files
mount -o remount,rw /system
rm -f /system/lib/modules/*
cp -fR /modules/*  /system/lib/modules
chmod -R 0644 system/lib/modules
find /system/lib/modules -type f -name '*.ko' -exec chown 0:0 {} \;

# make sure init.d is ok
chgrp -R 2000 /system/etc/init.d
chmod -R 777 /system/etc/init.d
sync

# force insert modules that are required
insmod /system/lib/modules/bcmdhd.ko
insmod /system/lib/modules/baseband_xmm_power2.ko
insmod /system/lib/modules/raw_ip_net.ko
insmod /system/lib/modules/baseband_usb_chr.ko
insmod /system/lib/modules/cdc_acm.ko
touch /data/local/em_modules_deployed
mount -o remount,ro /system

# run EliteKernel tweaks (overrides ROM tweaks)

# need to enable all CPU cores in order to set them up
echo 4 > /sys/power/pnpmgr/hotplug/min_on_cpus
sleep 2

# set governors
echo "ondemand" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor

# set default speeds (cpus activate in order 0-3-2-1)
echo "51000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq

# reset core activation to default
sleep 1 
echo 0 > /sys/power/pnpmgr/hotplug/min_on_cpus

# set ondemand prefs

# set vm tweaks

# minfree

#I/O tweaks
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /cache /cache
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /data /data
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /sd-ext /sd-ext
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /devlog /devlog

# activate delayed config to override ROM
/system/xbin/busybox nohup /system/bin/sh /elitekernel/elitekernel_delayed.sh 2>&1 >/dev/null &
