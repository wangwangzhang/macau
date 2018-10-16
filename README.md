## Source installation on Ubuntu
```bash
# install dependencies:
sudo apt-get install python-pip python-numpy python-scipy python-pandas cython
sudo apt-get install libopenblas-dev autoconf gfortran
sudo apt-get install libhdf5-serial-dev

# checkout and install Macau
git clone https://github.com/jaak-s/macau.git
cd macau
python setup.py install --user
```


