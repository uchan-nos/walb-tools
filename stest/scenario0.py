import os
from walb_cmd import *

s0 = Server('s0', '10000', None)
s1 = Server('s1', '10001', None)
s2 = Server('s2', '10002', None)
p0 = Server('p0', '10100', None)
p1 = Server('p1', '10101', None)
p2 = Server('p2', '10102', None)
a0 = Server('a0', '10200', 'vg0')
a1 = Server('a1', '10201', 'vg1')
#a2 = Server('a2', '10202', None)

config = Config(True, os.getcwd() + '/binsrc/', os.getcwd() + '/stest/tmp/', [s0, s1], [p0, p1], [a0, a1])
setConfig(config)

def main():
    devName = '/dev/walb/0'
    v0 = 'vol0'
    kill_all_servers()
    time.sleep(1)
    startup_all()
    init(s0, v0, devName)
    writeRandom(devName, 3)
    md0 = getSha1(devName)
    gid = full_backup(s0, v0)
    print "gid=", gid
    restore(a0, v0, gid)
    restoredPath = getRestoredPath(a0, v0, gid)
    print "restoredPath=", restoredPath
    md1 = getSha1(restoredPath)
    print "md0", md0
    print "md1", md1

if __name__ == "__main__":
    main()
