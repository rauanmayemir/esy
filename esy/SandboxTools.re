module StringMap = Map.Make(String);

module StringSet = Set.Make(String);

module PathSet = Set.Make(Path);

module ConfigPath = Config.ConfigPath;

let find = (~name, task: Task.t) => {
  let f = (task: Task.t) => task.pkg.name == name;
  Task.DependencyGraph.find(~f, task);
};

let getOcamlfind = (~cfg: Config.t, task: Task.t) =>
  RunAsync.Syntax.(
    switch (find(~name="@opam/ocamlfind", task)) {
    | None =>
      error(
        "We couldn't find ocamlfind, consider adding it to your devDependencies",
      )
    | Some(ocamlfindTask) =>
      let ocamlfindBin =
        ConfigPath.(
          ocamlfindTask.paths.installPath / "bin" / "ocamlfind" |> toPath(cfg)
        );
      let%bind built = Fs.exists(ocamlfindBin);
      if (built) {
        return(Path.to_string(ocamlfindBin));
      } else {
        /* TODO Autmatically build ocamlfind instead */
        error(
          "No ocamlfind binary was found, build your project first",
        );
      };
    }
  );

let getOcamlobjinfo = (~cfg: Config.t, task: Task.t) =>
  RunAsync.Syntax.(
    switch (find(~name="ocaml", task)) {
    | None =>
      error("We couldn't find ocaml, you need to add it to your dependencies")
    | Some(ocamlTask) =>
      let ocamlobjinfoBin =
        ConfigPath.(
          ocamlTask.paths.installPath / "bin" / "ocamlobjinfo" |> toPath(cfg)
        );
      let%bind built = Fs.exists(ocamlobjinfoBin);
      if (built) {
        return(Path.to_string(ocamlobjinfoBin));
      } else {
        /* TODO Automatically build ocaml instead */
        error(
          "OCaml hasn't been yet compiled, build your project first",
        );
      };
    }
  );

let splitBy = (line, ch) =>
  switch (String.index(line, ch)) {
  | idx =>
    let key = String.sub(line, 0, idx);
    let pos = idx + 1;
    let val_ = String.(trim(sub(line, pos, String.length(line) - pos)));
    Some((key, val_));
  | exception Not_found => None
  };

let getPackageLibraries =
    (~cfg: Config.t, ~ocamlfind: string, ~builtIns=?, ~task=?, ()) => {
  open RunAsync.Syntax;
  let ocamlpath =
    switch (task) {
    | Some((task: Task.t)) =>
      ConfigPath.(task.paths.installPath / "lib" |> toPath(cfg))
      |> Path.to_string
    | None => ""
    };
  let env =
    `CustomEnv(Astring.String.Map.(empty |> add("OCAMLPATH", ocamlpath)));
  let cmd = Cmd.ofList([ocamlfind, "list"]);
  let%bind out = ChildProcess.runOut(~env, cmd);
  let libs =
    String.split_on_char('\n', out)
    |> List.map(line => splitBy(line, ' '))
    |> Std.List.filterNone
    |> List.map(((key, _)) => key)
    |> List.rev;
  switch (builtIns) {
  | Some(discard) => return(Std.List.diff(libs, discard))
  | None => return(libs)
  };
};

type meta = {
  package: string,
  description: string,
  version: string,
  archive: string,
  location: string,
};

let queryMeta = (~cfg: Config.t, ~ocamlfind: string, ~task: Task.t, lib) => {
  open RunAsync.Syntax;
  let ocamlpath =
    ConfigPath.(task.paths.installPath / "lib" |> toPath(cfg))
    |> Path.to_string;
  let env =
    `CustomEnv(Astring.String.Map.(empty |> add("OCAMLPATH", ocamlpath)));
  let cmd =
    Cmd.ofList([
      ocamlfind,
      "query",
      "-predicates",
      "byte,native",
      "-long-format",
      lib,
    ]);
  let%bind out = ChildProcess.runOut(~env, cmd);
  let lines =
    String.split_on_char('\n', out)
    |> List.map(line => splitBy(line, ':'))
    |> Std.List.filterNone
    |> List.rev;
  let findField = (~name) =>
    lines
    |> List.map(((field, value)) => field == name ? Some(value) : None)
    |> Std.List.filterNone
    |> List.hd;
  return({
    package: findField(~name="package"),
    description: findField(~name="description"),
    version: findField(~name="version"),
    archive: findField(~name="archive(s)"),
    location: findField(~name="location"),
  });
};

let queryModules = (~ocamlobjinfo: string, archive) => {
  open RunAsync.Syntax;
  let env = `CustomEnv(Astring.String.Map.(empty));
  let cmd = Cmd.ofList([ocamlobjinfo, archive]);
  let%bind out = ChildProcess.runOut(~env, cmd);
  let startsWith = (s1, s2) => {
    let len1 = String.length(s1);
    let len2 = String.length(s2);
    len1 < len2 ? false : String.sub(s1, 0, len2) == s2;
  };
  let lines =
    String.split_on_char('\n', out)
    |> List.filter(line =>
         startsWith(line, "Name: ") || startsWith(line, "Unit name: ")
       )
    |> List.map(line => splitBy(line, ':'))
    |> Std.List.filterNone
    |> List.map(((_, val_)) => val_)
    |> List.rev;
  return(lines);
};

let formatPackageInfo = (~built: bool, task: Task.t) => {
  open RunAsync.Syntax;
  let pkg = task.pkg;
  let version = Chalk.grey("@" ++ pkg.version);
  let status =
    switch (pkg.sourceType, built) {
    | (Package.SourceType.Immutable, true) => Chalk.green("[built]")
    | (Package.SourceType.Immutable, false)
    | (_, _) => Chalk.blue("[build pending]")
    };
  let line = Printf.sprintf("%s%s %s", pkg.name, version, status);
  return(line);
};
