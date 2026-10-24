// Copyright (c) 2026 Magnominus
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package org.armgba.app;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;

import org.libsdl.app.SDLActivity;

// The frontend: SDL's activity, which loads libSDL2.so and libmain.so and runs its main, and Android's
// folder picker for the menus' folders.
public class Armgba extends SDLActivity {
	private static final int PICK_FOLDER = 1;
	// The folder picked: its path, "" if none was, or "?" if there's no picker; null while waiting.
	private static volatile String picked;

	// From the frontend's thread: shows the picker; takeFolder has its answer.
	public static void pickFolder() {
		final Armgba activity = (Armgba) mSingleton;
		activity.runOnUiThread(() -> {
			try {
				activity.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
			} catch (ActivityNotFoundException e) {
				picked = "?";
			}
		});
	}

	public static String takeFolder() {
		String folder = picked;
		picked = null;
		return folder;
	}

	@Override
	protected void onActivityResult(int request, int result, Intent data) {
		if (request != PICK_FOLDER) {
			super.onActivityResult(request, result, data);
			return;
		}
		picked = result == RESULT_OK && data != null ? pathOf(data.getData()) : "";
	}

	// The picker answers with a document tree, "primary:ROMs" for the shared storage's ROMs folder or
	// "<volume>:roms" for a card's; with all files access, its path is read directly.
	private static String pathOf(Uri tree) {
		if (tree == null || !"com.android.externalstorage.documents".equals(tree.getAuthority())) {
			return "";
		}
		String id = DocumentsContract.getTreeDocumentId(tree);
		int colon = id.indexOf(':');
		String volume = colon < 0 ? id : id.substring(0, colon);
		String inside = colon < 0 ? "" : id.substring(colon + 1);
		String root = volume.equals("primary") ? "/storage/emulated/0" : "/storage/" + volume;
		return inside.isEmpty() ? root : root + "/" + inside;
	}
}
