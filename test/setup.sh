#!/bin/bash

if command -v python
then
	python -m venv test_env
else
	python3 -m venv test_env
fi
