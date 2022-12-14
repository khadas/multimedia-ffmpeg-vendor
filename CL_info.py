import os
import sys
import json
str_cb  = ": CB"
str_cb0 = str_cb + "0"
str_cb1 = str_cb + "1"
str_cb2 = str_cb + "2"

str_cf  = ": CF"
str_cf0 = str_cf + "0"
str_cf1 = str_cf + "1"
str_cf2 = str_cf + "2"


cb0_count=0
cb1_count=0
cb2_count=0

cf0_count=0
cf1_count=0
cf2_count=0

cb_text=""
cb0_text=""
cb1_text=""
cb2_text=""

cf0_text=""
cf1_text=""
cf2_text=""


def resetResult() :
	global cb0_text
	global cb1_text
	global cb2_text
	global cb0_count
	global cb1_count
	global cb2_count
	global cf0_text
	global cf1_text
	global cf2_text
	global cf0_count
	global cf1_count
	global cf2_count
	cb0_count=0
	cb1_count=0
	cb2_count=0

	cf0_count=0
	cf1_count=0
	cf2_count=0

	cb_text=""
	cb0_text=""
	cb1_text=""
	cb2_text=""

	cf0_text=""
	cf1_text=""
	cf2_text=""

path = "tmp.txt"

def cmd_exe (cmd):
	process = os.popen(cmd) # return file
	output = process.readlines()
	process.close()
	return output

def show_help():
	print "Usage: CL_info [OPTION] ..."
	print ""
	print "OPTION:"
	print "	-v"
	print "		Statistics by Version"
	print "		ex: CL_info -v"
	print "		ex: CL_info -v VERSION_FILE"
	print "	-s"
	print "		Statistics by commit id"
	print "		ex: CL_info -s old_commit_id"
	print "		ex: CL_info -s old_commit_id new_commit_id"

def statistics_cb(str):
	global cb0_text
	global cb1_text
	global cb2_text
	global cb0_count
	global cb1_count
	global cb2_count


	if str.find(str_cb) != -1:
		if str.find(str_cb0) != -1:
			cb0_count+=1
			cb0_text=cb0_text + str
		elif str.find(str_cb1) != -1:
			cb1_count+=1
			cb1_text=cb1_text + str
		elif str.find(str_cb2) != -1:
			cb2_count+=1
			cb2_text=cb2_text + str

def statistics_cf(str):
	global cf0_text
	global cf1_text
	global cf2_text
	global cf0_count
	global cf1_count
	global cf2_count

	if str.find(str_cf) != -1:
		if str.find(str_cf0) != -1:
			cf0_count+=1
			cf0_text=cf0_text + str
		elif str.find(str_cf1) != -1:
			cf1_count+=1
			cf1_text=cf1_text + str
		elif str.find(str_cf2) != -1:
			cf2_count+=1
			cf2_text=cf2_text + str


def find_info_from_file(new_commit, older_commit):
	resetResult()
	if new_commit == "" :
		new_commit = "HEAD"
	if older_commit == "" :
		cmdStr = "git rev-list --first-parent --abbrev-commit --pretty=oneline "+ new_commit +" "
	else:
		cmdStr = "git rev-list --first-parent --abbrev-commit --pretty=oneline "+ older_commit +"^.."+ new_commit +" "
	#print(cmdStr)
	lines = cmd_exe(cmdStr)

	line=""
	for line in lines:
		#print(line)
		statistics_cb(line)
		statistics_cf(line)

	print("New feature: %d" % (cf0_count+cf1_count+cf2_count))
	print("   Critical feature: %d" % (cf0_count))
	print("   Important feature: %d" % (cf1_count))
	print("   Normal feature: %d" % (cf2_count))
	print cf0_text+cf1_text+cf2_text

	print("Fixed BUG: %d " % (cb0_count+cb1_count+cb2_count))
	print("   Fixed critical BUG: %d" % (cb0_count))
	print("   Fixed important BUG: %d" % (cb1_count))
	print("   Fixed normal BUG: %d" % (cb2_count))
	print cb0_text+cb1_text+cb2_text

def find_pre_commitid (older_commit) :
	lines = cmd_exe("git log |grep \"commit \"")
	commitid = ""
	for line in lines:
		line = line.rstrip()
		#print ("line :"+ line)
		if line.find("commit ") != -1:
			tmp = line.split(" ",3)
			#print ("------ :"+ str(tmp))
			if len(tmp[1]) == 40 :
				cid = tmp[1]
				if (commitid is not None) and len(commitid) == 40 and cid == older_commit:
					return commitid
				else :
					commitid = cid

def find_version_changeid(ver_file_path):
	new_commit = ""
	older_commit = ""
	version_commitid=""
	pre_changeid=""
	line=""
	count=0
	Major_V=""
	Minor_V=""
	Version=""
	if ver_file_path == "":
		ver_file_path = "version.json"
	f = open(ver_file_path)
	version_config = json.load(f)
	Major_V = version_config['version']['major']
	Minor_V = version_config['version']['minor']
	AML_VER = version_config['version']['amlogicver']
	print "Current Version: V"+str(Major_V)+"."+str(Minor_V)+"."+str(AML_VER)
	older_commit = version_config['version']['commit']
	older_commit = find_pre_commitid(older_commit)
	if older_commit is None:
		print ("find_pre_commitid failed.")
		exit()
	find_info_from_file(new_commit, older_commit)
	versions = version_config['history']
	totalVer = len(versions)
	for v in versions :
		new_commit = v['Release_CommitId']
		older_commit = v['Base_CommitID']
		print "****************************************"
		CommitCount = cmd_exe("git rev-list --first-parent --abbrev-commit --pretty=oneline "+older_commit+"^.."+new_commit+" |wc -l | sed -e 's/^ *//g'")
		print "Version:" + v['versionStr'] + "." + str(CommitCount[0])+ "-g"+str(new_commit)
		count = count+1
		if count < totalVer:
			find_info_from_file(new_commit, older_commit)
		else :
			find_info_from_file(new_commit, "")
	f.close()

def version_mode():
	if len(sys.argv) == 2:
		find_version_changeid("")
	elif len(sys.argv) == 3:
		find_version_changeid(sys.argv[2])
	else:
		print "parameter is wrong"
		show_help()
		sys.exit()

def check_parameter_mode():
	new_commit = ""
	older_commit = ""

	if len(sys.argv) > 1:
		if sys.argv[1] == "-s":
			if len(sys.argv) == 3:
				older_commit = sys.argv[2]
			elif len(sys.argv) == 4:
				new_commit = sys.argv[2]
				older_commit = sys.argv[3]
			else:
				print "parameter is wrong"
				show_help()
				sys.exit()
			find_info_from_file(new_commit, older_commit)
		elif sys.argv[1] == "-v":
			version_mode()
		elif sys.argv[1] == "-h":
			show_help()
			sys.exit()
		else:
			print "parameter is wrong"
			show_help()
			sys.exit()
	else:
		find_info_from_file("", "")

check_parameter_mode()

