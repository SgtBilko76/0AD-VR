package com.wildfiregames.zeroad;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * 0 A.D. VR for Meta Quest. SDL provides the Android lifecycle and window;
 * the game (libpyrogenesis.so) runs an OpenXR session on top of it.
 * Game data is read from /sdcard/0AD, which needs "All files access".
 */
public class ZeroADActivity extends SDLActivity
{
	@Override
	protected String[] getLibraries()
	{
		return new String[] {
			"c++_shared",
			"SDL2",
			"openal",
			"mozjs128-release",
			"openxr_loader",
			"pyrogenesis"
		};
	}

	@Override
	protected String getMainFunction()
	{
		return "pyrogenesis_main";
	}

	@Override
	protected String[] getArguments()
	{
		// Quest only supports the Vulkan backend (no desktop OpenGL).
		List<String> args = new ArrayList<>();
		args.add("-conf=rendererbackend:vulkan");
		// Developer testing: adb shell am start ... --es args "-autostart=..."
		String extra = getIntent() != null ? getIntent().getStringExtra("args") : null;
		if (extra != null && !extra.trim().isEmpty())
			args.addAll(Arrays.asList(extra.trim().split("\\s+")));
		return args.toArray(new String[0]);
	}

	@Override
	protected void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);
		if (mBrokenLibraries)
			return;

		File root = new File(Environment.getExternalStorageDirectory(), "0AD");
		nativeSetenv("ZEROAD_ANDROID_ROOT", root.getAbsolutePath());
		requestStorageAccess();
	}

	private void requestStorageAccess()
	{
		// Android 10 (e.g. Pico 4): legacy storage with runtime permissions.
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R &&
			checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) != android.content.pm.PackageManager.PERMISSION_GRANTED)
		{
			requestPermissions(new String[] {
				android.Manifest.permission.READ_EXTERNAL_STORAGE,
				android.Manifest.permission.WRITE_EXTERNAL_STORAGE }, 1);
			return;
		}
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager())
		{
			try
			{
				Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
				intent.setData(Uri.parse("package:" + getPackageName()));
				startActivity(intent);
			}
			catch (Exception e)
			{
				startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
			}
		}
	}

	@Override
	protected void onDestroy()
	{
		// The engine keeps threads (audio, network) alive after its main loop
		// returns. End the whole process once the activity is gone; the
		// watchdog covers a game thread that doesn't stop.
		Thread watchdog = new Thread(new Runnable()
		{
			@Override
			public void run()
			{
				try { Thread.sleep(3000); } catch (InterruptedException e) { return; }
				android.os.Process.killProcess(android.os.Process.myPid());
			}
		});
		watchdog.setDaemon(true);
		watchdog.start();

		super.onDestroy();
		android.os.Process.killProcess(android.os.Process.myPid());
	}
}
