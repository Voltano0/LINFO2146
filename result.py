# Open a txt file and retrieve data from it
file = open("energised/result.txt", "r")

# with the following line format : 1:46:39.448	ID:5	TREE : Node 5: broadcast rank 1
# Keep only value with time <= 1:30:00:000

# Read the file line by line
lines = file.readlines()
# Create a list to store the filtered lines
filtered_lines = []
# Iterate through each line in the file
for line in lines:
    # Split the line into parts
    parts = line.split("\t")
    # Check if the line has the expected number of parts
    if len(parts) >= 3:
        # Extract the time part and convert it to seconds
        time_part = parts[0]
        time_parts = time_part.split(":")
        if len(time_parts) == 3:
            hours = int(time_parts[0])
            minutes = int(time_parts[1])
            seconds = float(time_parts[2])
            total_seconds = hours * 3600 + minutes * 60 + seconds
            # Check if the total seconds is less than or equal to 5400 (1 hour 30 minutes)
            if total_seconds <= 7200:
                filtered_lines.append(line.strip())
# count the lines having "send reading" inside 
countsd = 0
counthello = 0
countV = 0
countV2 = 0
countS = 0
countL =0
for line in filtered_lines:
    if "send reading" in line:
        countsd += 1
    if "HELLO" in line:
        counthello += 1
    if "valve" in line:
        countV += 1
    if "Server got" in line:
        countS += 1
    countL += 1
# Print the counts
print("Count of 'send reading':", countsd)
print("Count of 'HELLO':", counthello)
print("Count of 'valve':", countV)
print("Count of 'Server got':", countS)
print("Count of all lines:", countL)
# Close the file
file.close()

#energyised
# Count of 'send reading': 208
# Count of 'HELLO': 5078
# Count of 'valve': 35
# Count of 'Server got': 70
# Count of all lines: 5737


#unenergised
# Count of 'send reading': 300
# Count of 'HELLO': 2776
# Count of 'valve': 30
# Count of 'Server got': 60
# Count of all lines: 5769