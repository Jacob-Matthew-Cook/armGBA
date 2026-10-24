// Copyright (c) 2026 Magnominus
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package org.armgba.app;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

// Asks once for access to the shared storage, where the games and armGBA's folder are, then starts
// the frontend (Armgba). Without it armGBA still starts, but sees only what Android lets it.
public class Start extends Activity {
	private boolean asked;

	@Override
	protected void onResume() {
		super.onResume();
		if (!asked && !canReadStorage()) {
			asked = true;
			if (Build.VERSION.SDK_INT >= 30) {
				startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
			} else {
				requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }, 0);
			}
			return;
		}
		begin();
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
		begin();
	}

	private boolean canReadStorage() {
		if (Build.VERSION.SDK_INT >= 30) {
			return Environment.isExternalStorageManager();
		}
		return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
	}

	private void begin() {
		startActivity(new Intent(this, Armgba.class));
		finish();
	}
}
