#!/system/bin/sh

sleep 60 # do the configuration again to override ROM and tegra hardcoded stuff

# need to enable all CPU cores in order to set them up
echo 4 > /sys/power/pnpmgr/hotplug/min_on_cpus
sleep 2

# set governors
echo "smartassV2" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo "smartassV2" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor
echo "smartassV2" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
echo "smartassV2" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor

# set default speeds (cpus activate in order 0-3-2-1)
echo "51000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq

# reset core activation to default
sleep 1
echo 0 > /sys/power/pnpmgr/hotplug/min_on_cpus

# set ondemand prefs again

# set vm tweaks again

# minfree again

touch /data/local/em_delayed_tweaks


