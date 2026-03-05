defmodule NervesSystemOrangepi5plus.MixProject do
  use Mix.Project

  @github_organization "vidarh"
  @app :nerves_system_orangepi5plus
  @version Path.join(__DIR__, "VERSION") |> File.read!() |> String.trim()

  def project do
    [
      app: @app,
      version: @version,
      elixir: "~> 1.15",
      compilers: Mix.compilers() ++ [:nerves_package],
      nerves_package: nerves_package(),
      deps: deps(),
      aliases: [loadconfig: [&bootstrap/1]]
    ]
  end

  defp bootstrap(args) do
    set_target()
    Application.start(:nerves_bootstrap)
    Mix.Task.run("loadconfig", args)
  end

  defp set_target do
    if function_exported?(Mix, :target, 1) do
      apply(Mix, :target, [:orangepi5plus])
    end
  end

  defp nerves_package do
    [
      type: :system,
      artifact_sites: [
        {:github_releases, "#{@github_organization}/#{@app}"}
      ],
      build_runner_opts: build_runner_opts(),
      platform: Nerves.System.BR,
      platform_config: [
        defconfig: System.get_env("NERVES_DEFCONFIG", "nerves_defconfig")
      ],
      # Environment for cross-compilation (same SoC as Rock 5T: RK3588)
      # A55 cores are binary compatible, kernel scheduler handles allocation
      env: [
        {"TARGET_ARCH", "aarch64"},
        {"TARGET_CPU", "cortex_a76"},
        {"TARGET_OS", "linux"},
        {"TARGET_ABI", "gnu"},
        {"TARGET_GCC_FLAGS",
         "-mabi=lp64 -fstack-protector-strong -mcpu=cortex-a76 -fPIE -pie -Wl,-z,now -Wl,-z,relro"}
      ],
      checksum: package_files()
    ]
  end

  defp deps do
    [
      {:nerves, "~> 1.11", runtime: false},
      {:nerves_system_br, "1.33.0", runtime: false},
      {:nerves_toolchain_aarch64_nerves_linux_gnu, "~> 13.2.0", runtime: false}
    ]
  end

  defp build_runner_opts do
    # CRITICAL: Limit to 4 parallel jobs to avoid running out of memory
    # during memory-intensive builds (WebKit, Chromium deps, etc.)
    [make_args: ["-j4"]]
  end

  defp package_files do
    [
      "nerves_defconfig",
      "linux-6.1-bsp.defconfig",
      "fwup.conf",
      "fwup-revert.conf",
      "rootfs_overlay",
      "post-build.sh",
      "post-createfs.sh",
      "cmdline.txt",
      "uboot.fragment",
      "VERSION",
      "mix.exs"
    ]
  end
end
