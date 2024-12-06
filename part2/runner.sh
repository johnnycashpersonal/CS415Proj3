#!/bin/bash


SCRIPT_TO_RUN="./bank input-1.txt"  # EDIT THIS WITH THE SCRIPT YOU WANT TO RUN. THIS IS HOW WE WILL TEST YOUR PROJ 3 FOR DEADLOCKS

# num times to run the script
COUNT=1000

for ((i=1; i<=COUNT; i++))
do
  echo "Running iteration $i"
  $SCRIPT_TO_RUN
done
