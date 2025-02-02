#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2019 Kyle Guinn <elyk03@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Script to download the calibration files for your camera,
# because direct access via FTP/HTTP would make too much sense.

if __name__ == '__main__':
	import argparse
	import os
	import http.client
	import json
	import re
	import shutil
	import time

	parser = argparse.ArgumentParser(description='''
		Downloads the calibration files for your camera.  All files
		will be stored in a subdirectory of the current directory,
		based on the camera's serial number.''')
	pgroup = parser.add_argument_group('JSON data', description='''
		Data fields provided in all requests to the server.  These
		fields were observed while downloading with version 2.6.25
		of the app; they may be subject to change and may not even
		be necessary.  All are sent as strings.''')
	# Get these first three from your camera.  To find the values,
	# watch the output of ThermAppCam when it starts.
	pgroup.add_argument('--serialNumber', required=True)
	pgroup.add_argument('--fWVersion')
	pgroup.add_argument('--hWVersion')
	# Don't know what the calibration type is, and it's not clear what
	# should be in appVersion when we're not using the official app.
	pgroup.add_argument('--calibType')  # Observed as '0'
	pgroup.add_argument('--appVersion')  # Observed as '2.6.25'
	# Everything below shouldn't have any effect on calibration data.
	# Probably just extra data collection for stats or debugging.
	pgroup.add_argument('--androidVersion')
	pgroup.add_argument('--phoneIMEI')
	pgroup.add_argument('--phoneHWVersion')
	pgroup.add_argument('--clientIP')
	args = parser.parse_args()

	# In addition to these headers, the app is sending:
	#   Connection: Keep-Alive
	#   User-Agent: Apache-HttpClient/UNAVAILABLE (Java/0)
	#
	# Python's http.client module auto-populates Host, Content-Length,
	# and Accept-Encoding.  The app only sends the first two of these.
	headers = {
		'Content-Type': 'application/json',
		'Accept': 'application/json',
	}

	# As compared to the app, there will be differences in:
	#   Capitalization (e.g. 'Content-type' with a lowercase 't')
	#   Order of headers and JSON data elements
	#   JSON formatting (the app omits all whitespace, may escape some
	#                    chars that don't need it, particularly '/')
	#
	# It may not be necessary to provide any of the JSON data fields in
	# requests, but the serial number is used by this script to select the
	# directory to download.  Provide at least that, but filter out all
	# items that are not given on the command line.
	body = { k:v for k,v in vars(args).items() if v is not None }

	# Create the output directory now, before starting the connection
	# (in case directory creation fails for some reason).
	if not os.path.isdir(body['serialNumber']):
		os.mkdir(body['serialNumber'])
	os.chdir(body['serialNumber'])

	conn = http.client.HTTPConnection('api.therm-app.com')
	#conn.set_debuglevel(1)

	# SessionStart returns a 'sessionID' (among other things) that will be
	# sent along with all subsequent requests and will appear in all
	# responses.  The sessionID appears to be a datetime string in the
	# server's localtime (+0300), down to the millisecond, and without any
	# punctuation nor leading zeros on each date and time element.
	# 
	# Other fields in each JSON response are 'data', 'errorMessage',
	# 'hasError', and 'ruleActions'.  Only 'data' appears to be useful,
	# and only for the GetFilesList/GetFile requests below.
	print('SessionStart')
	conn.request('POST', '/mobileservice.svc/SessionStart', body=json.dumps(body), headers=headers)
	resp = conn.getresponse()
	if resp.status != http.client.OK:
		raise ConnectionError('{0.status} {0.reason}'.format(resp))
	content_type = resp.getheader('Content-Type')
	if not content_type.startswith(headers['Accept']):
		raise RuntimeWarning('Content-Type: {0}'.format(content_type))
	data = json.load(resp)
	body['sessionID'] = data['sessionID']

	# GetFilesList returns a directory listing.
	# Request the serialNumber directory.
	body['data'] = { 'folder': body['serialNumber'] }
	print('GetFilesList', body['serialNumber'])
	conn.request('POST', '/mobileservice.svc/GetFilesList', body=json.dumps(body), headers=headers)
	resp = conn.getresponse()
	if resp.status != http.client.OK:
		raise ConnectionError('{0.status} {0.reason}'.format(resp))
	content_type = resp.getheader('Content-Type')
	if not content_type.startswith(headers['Accept']):
		raise RuntimeWarning('Content-Type: {0}'.format(content_type))
	data = json.load(resp)
	files = data['data']

	# Each element in the files array contains:
	#   'id':  An integer, appears to be sequential.
	#   'active':  A bool, unknown purpose.  Always observed as True.
	#   'createdByUserId':  A useless UUID.
	#   'createdDate':  A string: '/Date(x)/' where x is a timestamp.
	#   'folder':  Same as 'folder' in our previous request.
	#   'length':  Integer file length.
	#   'name':  File name string.
	#   'token':  Another UUID, used below when fetching the file.
	#   'type':  A string.  Always observed as 'bin'.
	#
	# The createdDate timestamp appears to be a Unix timestamp, suffixed
	# with milliseconds, then suffixed again with the UTC offset (+0300).
	re_date = re.compile('/Date\\(([0-9]+)([+-][0-9]+)?\\)/')

	# The app requests FilesList.json first.  Perhaps it uses it to filter
	# the files that need to be downloaded.  Instead, here we will
	# download everything in-order.
	#
	# The app leaves the Accept header as application/json during these
	# file fetches, but that seems inappropriate as the server sends it
	# as application/octet-stream.  Change it, then change back later.
	headers['Accept'] = 'application/octet-stream'
	for elem in files:
		# GetFile uses the 'token' UUID to request a file.
		body['data'] = { 'token': elem['token'] }
		print('GetFile', elem['name'])
		conn.request('POST', '/mobileservice.svc/GetFile', body=json.dumps(body), headers=headers)
		resp = conn.getresponse()
		if resp.status != http.client.OK:
			raise ConnectionError('{0.status} {0.reason}'.format(resp))
		content_type = resp.getheader('Content-Type')
		if not content_type.startswith(headers['Accept']):
			raise RuntimeWarning('Content-Type: {0}'.format(content_type))

		# Save the data to a file.
		f = open(elem['name'], 'wb')
		shutil.copyfileobj(resp, f)
		f.close()

		# Set its timestamp.
		timestamp = re_date.fullmatch(elem['createdDate'])
		if timestamp is not None:
			ns = int(timestamp[1]) * 1000000
			if timestamp.lastindex == 2:
				utc_offset = time.strptime(timestamp[2], '%z').tm_gmtoff
				ns -= utc_offset * 1000000000
			os.utime(elem['name'], ns=(ns, ns))
	headers['Accept'] = 'application/json'

	# End the session.
	body['data'] = None
	print('SessionEnd')
	conn.request('POST', '/mobileservice.svc/SessionEnd', body=json.dumps(body), headers=headers)
	resp = conn.getresponse()
	if resp.status != http.client.OK:
		raise ConnectionError('{0.status} {0.reason}'.format(resp))
	content_type = resp.getheader('Content-Type')
	if not content_type.startswith(headers['Accept']):
		raise RuntimeWarning('Content-Type: {0}'.format(content_type))
	data = json.load(resp)

	conn.close()
