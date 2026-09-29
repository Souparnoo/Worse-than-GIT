#!/bin/bash
sudo apt-get update
sudo apt-get install openssl -y
sudo apt-get install libssl-dev -y
mkdir -p ~/wtgit/bin
cp wtgit.sh ~/wtgit
cd ..
make
cd ~/wtgit/bin || echo "error"
chmod +x main
cd ..
if grep -q "source $PWD/wtgit.sh" "$PWD/../.bashrc" ; then
echo 'already installed bash source';
else
echo "source $PWD/wtgit.sh" >> ~/.bashrc;
fi
