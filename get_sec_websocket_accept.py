# -*- coding: utf-8 -*-
import sys
import hashlib
import base64
if len(sys.argv) == 2:
    key = sys.argv[1]
    sha1_hash = hashlib.sha1(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest()
    base64_encode = base64.b64encode(sha1_hash)
    print base64_encode
