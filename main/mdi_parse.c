/*
 * parse an mdi command
 *
 * valid commands and arguments:
 * 
 * STOP                 : stop the current sequence
 * START  filename.ext  : start the sequence file
 * START  label         : start the build-in sequence (previously loaded files too)
 * SERVO ch  value      : move the servo at ch to value
 * NEO   px  r g b      : set the pixel at px to the r g b values given
 * STRAND r g b         : set the whole strand to r g b
 * SCRIPT filename      : start the script file
 * NEXT                 : move to the next step in the script
 * PREVIOUS             : move to the previous step in the script
 * SYSINFO   where?
 * LIST
 * UPLOAD
 * DELETE
 * CAT filename  where?
 * 
 */