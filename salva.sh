#!/bin/bash
cd "$(dirname "$0")"

MESSAGGIO="${1:-Aggiornamento codice}"

git add .
git commit -m "$MESSAGGIO"
git push

echo "Backup completato su GitHub con messaggio: $MESSAGGIO"
