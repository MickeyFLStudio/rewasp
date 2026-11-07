; ReWasp installer (Inno Setup 6). Built by ..\build.bat.
; The work itself is done by rewasp.ps1, so the same logic can be run by hand.
#define AppVersion "1.0.3"

[Setup]
AppId={{6B0C5F0E-4E8A-4C35-9F5B-2A1D7F3C9E51}
AppName=ReWasp
AppVersion={#AppVersion}
AppPublisher=Pafu Igniters
AppPublisherURL=https://github.com/MickeyFLStudio/rewasp
AppSupportURL=https://github.com/MickeyFLStudio/rewasp/issues
DefaultDirName={autopf}\ReWasp
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=ReWasp-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
; dark, borderless look matching the artwork (brand\make_brand.py)
WizardStyle=modern dark hidebevels
WizardBackColor=#0B0B0F
DisableWelcomePage=no
UninstallDisplayName=ReWasp (Wasp for 64-bit FL Studio)
UninstallDisplayIcon={app}\rewasp.ico
SetupIconFile=brand\rewasp.ico
; Pafu Igniters / Mickey Perry artwork; Setup picks the size that fits the screen scaling
WizardImageFile=brand\wizard_small.png,brand\wizard.png
WizardSmallImageFile=brand\corner_small.png,brand\corner.png
WizardImageBackColor=#0B0B0F
CloseApplications=no

[Files]
Source: "..\build\x64\Wasp_x64.dll"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\build\x86\WaspHost32.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "rewasp.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "brand\rewasp.ico"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
WelcomeLabel1=ReWasp
WelcomeLabel2=The original Wasp synth, back in 64-bit FL Studio. Your old projects open with their Wasp sounds again.%n%nMade by Mickey Perry, aka Pafu Igniters (breakbeat, trance and drum & bass), who also does motion design and animation. I built ReWasp because Wasp is all over my old tracks. I hope it brings some of yours back too.%n%nClose FL Studio before you continue.
FinishedHeadingLabel=ReWasp is installed
; no shortcuts are created, so Setup shows FinishedLabelNoIcons
FinishedLabelNoIcons=Open FL Studio: Wasp now works in 64-bit, and old projects load with their Wasp sounds.%n%nAfter an FL Studio update, run this setup again.

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\rewasp.ps1"" -Action uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RestoreWasp"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\backup"
Type: files; Name: "{app}\rewasp.log"

[Code]
var
  WaspPage: TInputFileWizardPage;
  DetectedWasp: String;

// Finds the original 32-bit Wasp.dll before the wizard shows, so the "where is Wasp" page
// only appears when there is nothing usable on this PC.
procedure DetectWasp;
var
  Code: Integer;
  OutFile: String;
  Lines: AnsiString;
begin
  DetectedWasp := '';
  ExtractTemporaryFile('rewasp.ps1');
  OutFile := ExpandConstant('{tmp}\wasp-path.txt');
  if Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\rewasp.ps1') +
          '" -Action detect -Out "' + OutFile + '"', ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, Code)
     and (Code = 0) and LoadStringFromFile(OutFile, Lines) then
    DetectedWasp := Trim(String(Lines));
  // Set-Content writes a UTF-8 BOM
  if (Length(DetectedWasp) > 0) and (Ord(DetectedWasp[1]) = $FEFF) then Delete(DetectedWasp, 1, 1);
  if Copy(DetectedWasp, 1, 3) = #$EF#$BB#$BF then Delete(DetectedWasp, 1, 3);
end;

procedure OpenLink(Sender: TObject);
var
  Code: Integer;
begin
  ShellExec('open', TNewStaticText(Sender).Hint, '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

// links under the finish text: the music, then the motion design / animation work
procedure AddLink(var Y: Integer; const Caption, Url: String; Heading: Boolean);
var
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(WizardForm);
  L.Parent := WizardForm.FinishedPage;
  L.Left := WizardForm.FinishedLabel.Left;
  L.Top := Y;
  L.Caption := Caption;
  if Heading then begin
    L.Font.Style := [fsBold];
    Y := Y + L.Height + ScaleY(4);
  end else begin
    L.Hint := Url;
    L.Cursor := crHand;
    L.Font.Color := $872DFF;   // #ff2d87, mickeyperry.github.io pink (BGR)
    L.Font.Style := [fsUnderline];
    L.OnClick := @OpenLink;
    L.Left := L.Left + ScaleX(12);
    Y := Y + L.Height + ScaleY(2);
  end;
end;

procedure AddLinks;
var
  Y: Integer;
begin
  Y := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(24);
  AddLink(Y, 'Pafu Igniters (music)', '', True);
  AddLink(Y, 'SoundCloud', 'https://soundcloud.com/pafu', False);
  AddLink(Y, 'Bandcamp', 'https://pafu.bandcamp.com', False);
  Y := Y + ScaleY(12);
  AddLink(Y, 'Mickey Perry (motion design and animation)', '', True);
  AddLink(Y, 'mickeyperry.github.io', 'https://mickeyperry.github.io/', False);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then AddLinks;
end;

procedure InitializeWizard;
begin
  DetectWasp;
  WaspPage := CreateInputFilePage(wpWelcome,
    'Original Wasp', 'ReWasp could not find the original 32-bit Wasp on this PC',
    'ReWasp runs Image-Line''s original 32-bit Wasp inside 64-bit FL Studio. It comes with FL Studio 12 or ' +
    'older (Plugins\Fruity\Generators\Wasp\Wasp.dll, next to its Artwork folder); the one from FL Studio 20 ' +
    'isn''t supported yet.' + #13#10#13#10 +
    'Install an older FL Studio alongside your current one and run this setup again, or pick a Wasp.dll:');
  WaspPage.Add('Wasp.dll:', 'Wasp.dll|Wasp.dll|All DLLs|*.dll', '.dll');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = WaspPage.ID) and (DetectedWasp <> '');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = WaspPage.ID) and not FileExists(WaspPage.Values[0]) then begin
    MsgBox('Pick the original Wasp.dll first.', mbError, MB_OK);
    Result := False;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Src: String;
begin
  Src := DetectedWasp;
  if Src = '' then Src := WaspPage.Values[0];
  Result := 'Original Wasp:' + NewLine + Space + Src + NewLine + NewLine +
            'ReWasp will be installed into every 64-bit FL Studio' + NewLine + 'on this PC that has Image-Line''s Wasp placeholder.';
end;

function RunScript(const Args: String): Integer;
var
  Code: Integer;
begin
  if not Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\rewasp.ps1') + '" ' + Args,
              ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Result := Code;
end;

function ReadLog: String;
var
  Lines: AnsiString;
begin
  Result := '';
  if LoadStringFromFile(ExpandConstant('{app}\rewasp.log'), Lines) then
  begin
    // PowerShell writes the log as UTF-8 with a byte-order mark; drop the mark
    if (Length(Lines) >= 3) and (Lines[1] = #$EF) and (Lines[2] = #$BB) and (Lines[3] = #$BF) then Delete(Lines, 1, 3);
    Result := String(Lines);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Args: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  // found automatically: let the script find it again (the path read back here is for display
  // only and may lose non-ASCII characters); picked by hand: pass it on
  Args := '-Action install';
  if DetectedWasp = '' then Args := Args + ' -WaspDll "' + WaspPage.Values[0] + '"';
  repeat
    Code := RunScript(Args);
    if Code = 2 then
      if MsgBox('FL Studio is running. Close it, then click Retry.', mbError, MB_RETRYCANCEL) = IDCANCEL then Break;
  until Code <> 2;
  if WizardSilent then Exit;   // the log says what happened
  if Code = 0 then
    MsgBox(ReadLog, mbInformation, MB_OK)
  else
    MsgBox('ReWasp could not finish:' + #13#10#13#10 + ReadLog + #13#10#13#10 +
           'Fix the problem and run this setup again.', mbError, MB_OK);
end;
