"""Embed only our compiled renderer in the manually launched USB payload."""
from pathlib import Path
import sys
data=Path(sys.argv[1]).read_bytes()
rows=[','.join(str(x) for x in data[i:i+32]) for i in range(0,len(data),32)]
Path(sys.argv[2]).write_text('const unsigned char home_bridge_blob[]={\n'+',\n'.join(rows)+
                           '\n};\nconst unsigned int home_bridge_blob_size='+str(len(data))+';\n')
