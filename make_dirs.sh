#!/bin/bash
set -e
 
echo "Creating volume directories..."
mkdir -p grafana timescale_data pgadmin_data
 
echo "Setting ownership..."
sudo chown -R 1000:1000 grafana
sudo chown -R 70:70 timescale_data
sudo chown -R 5050:0 pgadmin_data
 
echo "Done!"
 
