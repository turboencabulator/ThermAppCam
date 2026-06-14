#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2019 Kyle Guinn <elyk03@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Script to download the calibration files for your camera,
# because direct access via FTP/HTTP would make too much sense.
# More details at http://api.therm-app.com/MobileService.svc/help

import argparse
import os
import http.client
import json
import re
import shutil
import time

def json_request(conn, endpoint, body, headers):
	# The app uses lowercase 'mobileservice.svc'.
	conn.request('POST', '/MobileService.svc/' + endpoint, body=json.dumps(body), headers=headers)
	resp = conn.getresponse()
	if resp.status != http.client.OK:
		raise ConnectionError('{0.status} {0.reason}'.format(resp))

	content_type = resp.getheader('Content-Type')
	if not content_type.startswith(headers['Accept']):
		raise RuntimeWarning('Content-Type: {0}'.format(content_type))

	if content_type.startswith('application/json'):
		# JSON responses are expected to contain 'data', 'errorMessage',
		# 'hasError', 'ruleActions', and 'sessionID'.
		resp = json.load(resp)
		# Even HTTP 200 responses can contain embedded errors.
		if resp.get('hasError', False):
			raise ConnectionError(resp.get('errorMessage', ''))

	return resp

if __name__ == '__main__':
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
	pgroup.add_argument('--serialNumber', required=True, help='''
		words 5 and 6 from the frame header''')
	pgroup.add_argument('--hWVersion', help='''
		word 7 from the frame header''')
	pgroup.add_argument('--fWVersion', help='''
		word 8 from the frame header, if 256 then set this to 7''')
	# The app populates the calibration type with the version format
	# field (word 0) of 0.bin.  Note that 0.bin also contains a
	# calibration type field (as word 2).  Also note the circular
	# dependency of needing calibration files to populate a request
	# for calibration files.  The app populates with '0' when unknown.
	pgroup.add_argument('--calibType', help='''
		word 0 (version format) from 0.bin''')  # Observed as '0'
	# The app sends 'N/A' for appVersion and phoneIMEI when unknown.
	# It's not clear how appVersion should be used if we're not using
	# the official app.
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
	print('SessionStart')
	resp = json_request(conn, 'SessionStart', body=body, headers=headers)
	body['sessionID'] = resp['sessionID']

	# GetFilesList returns a directory listing.
	# Request the serialNumber directory.
	# body['serialNumber'] is also required by the server; it is checked
	# for validity but it doesn't need to match the requested directory.
	body['data'] = { 'folder': body['serialNumber'] }
	print('GetFilesList', body['serialNumber'])
	resp = json_request(conn, 'GetFilesList', body=body, headers=headers)
	files = resp['data']
	# Save the listing for posterity; it contains more info than FilesList.json.
	with open('GetFilesList.json', 'w') as f:
		json.dump(files, f, indent="\t")

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
	# with milliseconds, then suffixed again with the UTC offset (+0300)
	# to capture the location info.  This is a Microsoft .NET format.
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
		resp = json_request(conn, 'GetFile', body=body, headers=headers)

		# Save the response to a file.
		with open(elem['name'], 'wb') as f:
			shutil.copyfileobj(resp, f)

		# Set its timestamp.
		timestamp = re_date.fullmatch(elem['createdDate'])
		if timestamp is not None:
			ns = int(timestamp[1]) * 1000000
			if timestamp.lastindex == 2:
				utc_offset = time.strptime(timestamp[2], '%z').tm_gmtoff
			os.utime(elem['name'], ns=(ns, ns))
	headers['Accept'] = 'application/json'

	# End the session.
	del body['data']
	print('SessionEnd')
	resp = json_request(conn, 'SessionEnd', body=body, headers=headers)
	conn.close()
