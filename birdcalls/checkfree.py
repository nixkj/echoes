import os

# Get filesystem stats
stat = os.statvfs('/')

# Calculate free, total, and used space in bytes
free = stat[0] * stat[3]     # f_bsize * f_bfree
total = stat[0] * stat[2]    # f_bsize * f_blocks
used = total - free

print("Total:", total, "bytes")
print("Used:", used, "bytes")
print("Free:", free, "bytes")
