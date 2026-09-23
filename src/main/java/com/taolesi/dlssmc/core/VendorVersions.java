package com.taolesi.dlssmc.core;

/**
 * 构建期打进 jar 的厂商二进制版本。
 *
 * 这些字符串是给设置页显示「内置版本」用的，必须与 native/vendor/MANIFEST.sha256 里的文件一致；
 * 重新内置厂商 DLL 时同步改这里。实机跑的是哪个版本一律运行时查（见 FGRuntime#runtimeVersionOf），
 * 因为 FSR 那份 4.0.1 的包在非 RDNA4 显卡上只会给出 3.1.6 的 provider，写死就会骗人。
 */
public final class VendorVersions {
    /** amd_fidelityfx_framegeneration_dx12.dll 的文件版本（FidelityFX SDK 2.3.0 的预编译产物） */
    public static final String FSR_FRAME_GENERATION = "4.0.1.2740";
    /** libxess_fg.dll 的文件版本（XeSS SDK 3.0.2） */
    public static final String XESS_FRAME_GENERATION = "1.3.1.78";
    /** libxell.dll 的文件版本（XeSS SDK 3.0.2 内的 XeLL 1.3.2） */
    public static final String XELL = "1.3.2";
    public static final String FSR_SDK = "2.3.0";
    public static final String XESS_SDK = "3.0.2";
    /** DLSS 不内置：Streamline 由配置里的 streamlinePath 指向本机目录 */
    public static final String DLSS = "Streamline 2.14.1（外部目录）";

    private VendorVersions() {}
}
