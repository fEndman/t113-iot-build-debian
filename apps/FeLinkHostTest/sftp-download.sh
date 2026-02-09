#!/bin/sh

sftp $1@$2 << EOF
mput $3 $4
quit
EOF