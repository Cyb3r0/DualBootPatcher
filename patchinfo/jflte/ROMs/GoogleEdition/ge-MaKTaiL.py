from multiboot.fileinfo import FileInfo
import multiboot.autopatcher as autopatcher
import re

file_info = FileInfo()

filename_regex           = r"^i9505-ge-untouched-4.3-.*.zip$"
file_info.name           = "MaKTaiL's Google Edition"
file_info.ramdisk        = 'jflte/GoogleEdition/GoogleEdition.def'
file_info.patch          = autopatcher.auto_patch
file_info.extract        = autopatcher.files_to_auto_patch

def matches(filename):
  if re.search(filename_regex, filename):
    return True
  else:
    return False

def get_file_info():
  return file_info
