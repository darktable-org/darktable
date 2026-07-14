package net.darktable.mobile;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;
import androidx.core.content.FileProvider;
import java.io.File;
import java.util.ArrayList;

public class ShareHelper {
    private static final String TAG = "ShareHelper";

    public static void shareImages(Context context, String[] paths) {
        String authority = context.getPackageName() + ".fileprovider";
        ArrayList<Uri> uris = new ArrayList<>();

        for (String path : paths) {
            File file = new File(path);
            if (!file.exists()) {
                Log.w(TAG, "share: file not found: " + path);
                continue;
            }
            try {
                uris.add(FileProvider.getUriForFile(context, authority, file));
            } catch (Exception e) {
                Log.e(TAG, "share: failed to get URI for " + path, e);
            }
        }

        if (uris.isEmpty()) return;

        Intent intent;
        if (uris.size() == 1) {
            intent = new Intent(Intent.ACTION_SEND);
            intent.setType("image/jpeg");
            intent.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        } else {
            intent = new Intent(Intent.ACTION_SEND_MULTIPLE);
            intent.setType("image/jpeg");
            intent.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

        Intent chooser = Intent.createChooser(intent, "Share images");
        chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(chooser);
    }
}
