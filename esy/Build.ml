let waitForDependencies dependencies =
  dependencies
  |> List.map (fun (_, dep) -> dep)
  |> RunAsync.waitAll

let buildPackage ?(quiet=false) ?force ?stderrout ~buildOnly cfg (task : Task.t) =
  let f () =
    let open RunAsync.Syntax in
    let context = Printf.sprintf "building %s@%s" task.pkg.name task.pkg.version in
    let%lwt () = if not quiet
      then Logs_lwt.app(fun m -> m "%s: starting" context)
      else Lwt.return ()
    in
    let%bind () = RunAsync.withContext context (
      PackageBuilder.build ~quiet ?stderrout ?force ~buildOnly cfg task
    ) in
    let%lwt () = if not quiet
      then Logs_lwt.app(fun m -> m "%s: complete" context)
      else Lwt.return ()
    in
    return ()
  in
  let label = Printf.sprintf "building %s" task.pkg.id in
  Perf.measureTime ~label f

let runTask
  ?(force=`ForRoot)
  ?(buildOnly=`ForRoot)
  ~queue
  ~allDependencies:_
  ~dependencies
  (cfg : Config.t)
  (rootTask : Task.t)
  (task : Task.t) =

  let open RunAsync.Syntax in

  let%bind () = waitForDependencies dependencies in

  let isRoot = task.id == rootTask.id in
  let installPath = Config.ConfigPath.toPath cfg task.paths.installPath in

  let buildOnly = match buildOnly with
  | `ForRoot -> isRoot
  | `No -> false
  | `Yes -> true
  in

  let checkSourceModTime () =
    let f () =
      let infoPath =
        task.paths.buildInfoPath
        |> Config.ConfigPath.toPath cfg
      and sourcePath =
        task.paths.sourcePath
        |> Config.ConfigPath.toPath cfg
      in
      match%lwt Fs.readFile infoPath with
      | Ok data ->
        let%bind buildInfo = RunAsync.liftOfRun (
          Json.parseStringWith EsyBuildPackage.BuildInfo.of_yojson data
        ) in
        begin match buildInfo.EsyBuildPackage.BuildInfo.sourceModTime with
        | None -> buildPackage ~buildOnly cfg task
        | Some buildMtime ->
          let skipTraverse path = match Path.basename path with
          | "node_modules"
          | "_esy"
          | "_release"
          | "_build"
          | "_install" -> true
          | _ -> false
          in
          let f mtime _path stat =
            Lwt.return (
              if stat.Unix.st_mtime > mtime
              then stat.Unix.st_mtime
              else mtime
            )
          in
          let%lwt curMtime = Fs.fold ~skipTraverse ~f ~init:0.0 sourcePath in
          if curMtime > buildMtime
          then buildPackage ~buildOnly cfg task
          else return ()
        end
      | Error _ -> buildPackage ~buildOnly cfg task
    in
    let label = Printf.sprintf "checking mtime for %s" task.pkg.id in
    Perf.measureTime ~label f
  in

  let performBuildIfNeeded () =
    let f () =
    match task.pkg.sourceType with
    | Package.SourceType.Immutable ->
      if%bind Fs.exists installPath
      then return ()
      else buildPackage ~buildOnly cfg task
    | Package.SourceType.Development ->
      if%bind Fs.exists installPath
      then checkSourceModTime ()
      else buildPackage ~buildOnly cfg task
    | Package.SourceType.Root ->
      buildPackage ~buildOnly cfg task
    in LwtTaskQueue.submit queue f
  in

  match force with
  | `ForRoot ->
    if isRoot
    then buildPackage ~force:true ~buildOnly cfg task
    else performBuildIfNeeded ()
  | `No ->
    performBuildIfNeeded ()
  | `Yes ->
    buildPackage ~force:true ~buildOnly cfg task

(**
 * Build task tree.
 *)
let build
    ?force
    ?buildOnly
    ~concurrency
    (cfg : Config.t)
    (rootTask : Task.t)
    =
  let queue = LwtTaskQueue.create ~concurrency () in
  let f = runTask ?force ?buildOnly ~queue cfg rootTask in
  Task.DependencyGraph.foldWithAllDependencies ~f rootTask

(**
 * Build only dependencies of the task but not the task itself.
 *)
let buildDependencies
    ?force
    ?buildOnly
    ~concurrency
    (cfg : Config.t)
    (rootTask : Task.t)
    =
  let open RunAsync.Syntax in
  let queue = LwtTaskQueue.create ~concurrency () in
  let f ~allDependencies ~dependencies (task : Task.t) =
    if task.id = rootTask.id
    then (
      let%bind () = waitForDependencies dependencies in
      return ()
    ) else runTask ?force ?buildOnly ~allDependencies ~dependencies ~queue cfg rootTask task
  in
  Task.DependencyGraph.foldWithAllDependencies ~f rootTask
