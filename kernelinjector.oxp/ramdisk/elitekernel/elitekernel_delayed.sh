#!/system/bin/sh

sleep 60 # do the configuration again to override ROM and tegra hardcoded stuff

# need to enable all CPU cores in order to set them up
echo 4 > /sys/power/pnpmgr/hotplug/min_on_cpus
sleep 2

# set governors
echo "ondemand" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
echo "ondemand" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor

# set default speeds (cpus activate in order 0-3-2-1)
echo "1700000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo "1700000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq
echo "1700000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
echo "1700000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq

echo "51000" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
echo "51000" > /sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq

# reset core activation to default
sleep 1 

# set ondemand prefs again
#echo "80" > /sys/devices/system/cpu/cpufreq/ondemand/up_threshold
#echo "15" > /sys/devices/system/cpu/cpufreq/ondemand/down_differential
#echo "1" > /sys/devices/system/cpu/cpufreq/ondemand/ignore_nice_load
#echo "3000000" > /sys/devices/system/cpu/cpufreq/ondemand/input_boost_duration
#echo "1" > /sys/devices/system/cpu/cpufreq/ondemand/io_is_busy
#echo "5" > /sys/devices/system/cpu/cpufreq/ondemand/powersave_bias
#echo "5" > /sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor
#echo "30000" > /sys/devices/system/cpu/cpufreq/ondemand/sampling_rate
#echo "10000" > /sys/devices/system/cpu/cpufreq/ondemand/sampling_rate_min
#echo "1" > /sys/devices/system/cpu/cpufreq/ondemand/touch_poke
#echo "51000" > /sys/devices/system/cpu/cpufreq/ondemand/two_phase_bottom_freq
#echo "1" > /sys/devices/system/cpu/cpufreq/ondemand/two_phase_dynamic
#echo "340000" > /sys/devices/system/cpu/cpufreq/ondemand/two_phase_freq
#echo "3" > /sys/devices/system/cpu/cpufreq/ondemand/ui_counter
#echo "20000" > /sys/devices/system/cpu/cpufreq/ondemand/ui_sampling_rate
#echo "66" > /sys/devices/system/cpu/cpufreq/ondemand/ux_boost_threshold
#echo "760000" > /sys/devices/system/cpu/cpufreq/ondemand/ux_freq
#echo "20" > /sys/devices/system/cpu/cpufreq/ondemand/ux_loading

touch /data/local/em_delayed_tweaks
