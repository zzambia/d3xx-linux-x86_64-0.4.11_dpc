import os
import struct

fin = open("dumpfile.dump", "rb")

idx = 0
oldact = 0
act = 0

while True:
	oldact = act
	actstr = struct.unpack('h', fin.read(2))[0]
	act = int(actstr)
	'''
	if (act - oldact) > 1 and idx > 0:
		if not ((act == 0) and (oldact == 0xFFFF)):
			print("Wrong alignment "), idx
			print("Act "), (act, format(act, '#04x'))
			print("Act-1 "), (oldact, format(oldact, '#04x'))
			#break 
	'''
	#if (idx % 1024) == 0:
	
	if (((act-oldact) != 1) and ((act-oldact) != -127) and ((act-oldact) != 0)):
        
		print idx," -> ",act,"-",oldact,"=",(act-oldact)
		#break
	idx += 1


