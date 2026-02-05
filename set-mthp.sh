#!/bin/bash

# Note: 8KB pages can't be disabled
# Function to disable all MTHP page sizes and enable a specific one
disable_and_enable_mthp() {
	local page_order=$1

	# Array of available huge page sizes
	local PAGE_SIZES=(16 32 64 128 256 512 1024 2048)

	# Disable all MTHP page sizes
	for size in "${PAGE_SIZES[@]}"; do
		echo "never" > "/sys/kernel/mm/transparent_hugepage/hugepages-${size}kB/enabled"
	done

	# Enable a specific page size based on user input
	if [[ -n "$page_order" && "$page_order" =~ ^[2-9]$ ]]; then
		local index=$(( page_order - 2 ))
		if [[ $index -lt ${#PAGE_SIZES[@]} ]]; then
			local size=${PAGE_SIZES[$index]}
			echo "inherit" > "/sys/kernel/mm/transparent_hugepage/hugepages-${size}kB/enabled"
			echo "Enabled hugepage size: ${size}kB"
		else
			echo "Invalid page order. Must be between 2 and ${#PAGE_SIZES[@]}"
		fi
	else
		echo "Usage: disable_and_enable_mthp <page_order (2-9)>"
		return 1
	fi
}

if [[ "$1" == "0" ]]; then
	echo "Disable hugepages: 4kB pages"
	# Array of available huge page sizes
	PAGE_SIZES=(16 32 64 128 256 512 1024 2048)

	# Disable all MTHP page sizes
	for size in "${PAGE_SIZES[@]}"; do
		echo "never" > "/sys/kernel/mm/transparent_hugepage/hugepages-${size}kB/enabled"
	done
	echo never > /sys/kernel/mm/transparent_hugepage/enabled
else
	disable_and_enable_mthp $1
	echo always > /sys/kernel/mm/transparent_hugepage/enabled
fi
