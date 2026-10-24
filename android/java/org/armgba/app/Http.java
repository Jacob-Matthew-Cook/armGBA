// Copyright (c) 2026 Magnominus
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package org.armgba.app;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// RetroAchievements' requests on Android, which has no libcurl, through its own HTTPS; called from
// the achievements' worker thread (frontend/cheevos.c).
public final class Http {
	private Http() {
	}

	// The answer's body, with its HTTP code in status[0], or null when the server can't be reached.
	public static byte[] fetch(String url, byte[] post, String contentType, String userAgent, int[] status) {
		HttpURLConnection connection = null;
		try {
			connection = (HttpURLConnection) new URL(url).openConnection();
			connection.setConnectTimeout(30000);
			connection.setReadTimeout(30000);
			connection.setInstanceFollowRedirects(true);
			connection.setRequestProperty("User-Agent", userAgent);
			if (post != null) {
				connection.setDoOutput(true);
				connection.setRequestMethod("POST");
				connection.setRequestProperty("Content-Type", contentType);
				connection.setFixedLengthStreamingMode(post.length);
				OutputStream out = connection.getOutputStream();
				out.write(post);
				out.close();
			}
			status[0] = connection.getResponseCode();
			InputStream in = status[0] >= 400 ? connection.getErrorStream() : connection.getInputStream();
			ByteArrayOutputStream body = new ByteArrayOutputStream();
			if (in != null) {
				byte[] buffer = new byte[8192];
				for (int n; (n = in.read(buffer)) > 0;) {
					body.write(buffer, 0, n);
				}
				in.close();
			}
			return body.toByteArray();
		} catch (IOException e) {
			return null;
		} finally {
			if (connection != null) {
				connection.disconnect();
			}
		}
	}
}
