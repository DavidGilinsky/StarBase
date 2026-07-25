// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          pjsr/StarBaseWBPP.js
// Purpose:       StarBase "REST pull" helper for PixInsight. Fetches the frame
//                paths of a StarBase saved query over HTTP and hands them to
//                Weighted Batch Preprocessing (WBPP), so you can pull a set into
//                PixInsight by id without leaving the application.
// Created:       2026-07-25
// Last Modified: 2026-07-25
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// This is StarBase's own code. It talks to the StarBase REST API and to
// PixInsight's public PJSR objects; it does not vendor or modify any PixInsight
// or WBPP source, so its GPL license and PixInsight's PCL License 2.0 stay at
// arm's length.
//
// Run it from inside PixInsight: Script > Execute Script File..., or drop it in
// a scripts directory. It reads three settings (base URL, saved-query id, and
// an optional API token), calls
//
//     GET <base>/api/v1/queries/<id>/paths?format=text
//
// and adds the returned frames to a new WBPP instance opened for review.
//
// The daemon's TLS certificate is self-signed, so either point BASE_URL at the
// plain-HTTP bind of a trusted LAN, or import the StarBase certificate into the
// host trust store first. NetworkTransfer will refuse an untrusted HTTPS cert.
// ---------------------------------------------------------------------------

#feature-id    StarBase > Pull Query into WBPP

#include <pjsr/DataType.jsh>

#define SB_TITLE "StarBase - Pull Query into WBPP"

// ---- Settings, persisted between runs via the PixInsight Settings store -----

function SBSettings() {
   this.baseUrl = "http://localhost:8642";
   this.queryId = 1;
   this.token   = "";

   this.load = function() {
      let v;
      if ( (v = Settings.read( "StarBase/baseUrl", DataType_UCString )) != null ) this.baseUrl = v;
      if ( (v = Settings.read( "StarBase/queryId", DataType_Int32   )) != null ) this.queryId = v;
      if ( (v = Settings.read( "StarBase/token",   DataType_UCString )) != null ) this.token   = v;
   };
   this.save = function() {
      Settings.write( "StarBase/baseUrl", DataType_UCString, this.baseUrl );
      Settings.write( "StarBase/queryId", DataType_Int32,    this.queryId );
      Settings.write( "StarBase/token",   DataType_UCString, this.token );
   };
}

// ---- HTTP GET of the newline-delimited path list ---------------------------

function fetchPaths( settings ) {
   let url = settings.baseUrl.replace( /\/+$/, "" ) +
             "/api/v1/queries/" + settings.queryId + "/paths?format=text";

   let body = "";
   let transfer = new NetworkTransfer;
   transfer.setURL( url );
   if ( settings.token.length > 0 )
      transfer.setCustomHTTPHeaders( [ "Authorization: Bearer " + settings.token ] );
   transfer.onDownloadDataAvailable = function( data ) {
      body += data.utf8ToString();
      return true;
   };
   transfer.download();

   if ( !transfer.responseCode || transfer.responseCode >= 400 )
      throw new Error( "StarBase request failed (HTTP " + transfer.responseCode +
                       ") for " + url );

   let paths = [];
   let lines = body.split( "\n" );
   for ( let i = 0; i < lines.length; ++i ) {
      let p = lines[i].trim();
      if ( p.length > 0 )
         paths.push( p );
   }
   return paths;
}

// ---- Hand the frames to WBPP ------------------------------------------------
//
// WBPP exposes its stacking engine globally once its script has been loaded.
// We add the frames as light frames and open the dialog for review rather than
// running unattended, which is the safe default for an interactive pull.

function handToWBPP( paths ) {
   // The WBPP script registers a global StackEngine constructor. If it is not
   // present, WBPP has not been loaded in this session.
   if ( typeof StackEngine == "undefined" ) {
      throw new Error( "WBPP is not loaded. Open Script > Batch Processing > " +
                       "WeightedBatchPreprocessing once, then run this again." );
   }

   let engine = new StackEngine;
   if ( typeof engine.addLightFrames == "function" )
      engine.addLightFrames( paths );
   else if ( typeof engine.addFiles == "function" )
      engine.addFiles( paths );          // older WBPP: adds and auto-classifies
   else
      throw new Error( "This WBPP build exposes no addLightFrames/addFiles entry." );

   // Open the WBPP dialog pre-populated. The exact dialog constructor name has
   // been stable across recent WBPP releases.
   if ( typeof StackDialog != "undefined" ) {
      let dlg = new StackDialog( engine );
      dlg.execute();
   } else {
      console.warningln( "WBPP dialog constructor not found; frames were added " +
                         "to a StackEngine but no dialog was opened." );
   }
}

// ---- A small input dialog ---------------------------------------------------

function SBDialog( settings ) {
   this.__base__ = Dialog;
   this.__base__();
   let self = this;

   this.windowTitle = SB_TITLE;
   this.minWidth = 460;

   this.info = new Label( this );
   this.info.wordWrapping = true;
   this.info.useRichText = true;
   this.info.text = "<b>" + SB_TITLE + "</b><br>Pull a StarBase saved query's " +
                    "frames into a new WBPP instance.";

   this.urlEdit = new Edit( this );
   this.urlEdit.text = settings.baseUrl;
   this.urlEdit.toolTip = "StarBase base URL, e.g. http://localhost:8642";

   this.idEdit = new SpinBox( this );
   this.idEdit.minValue = 1;
   this.idEdit.maxValue = 1000000;
   this.idEdit.value = settings.queryId;
   this.idEdit.toolTip = "Saved-query id (see the Queries tab in the StarBase UI)";

   this.tokenEdit = new Edit( this );
   this.tokenEdit.text = settings.token;
   this.tokenEdit.toolTip = "Optional API token (SB_API_TOKEN); leave blank on a trusted LAN";

   function row( labelText, control ) {
      let s = new HorizontalSizer;
      s.spacing = 6;
      let l = new Label( self );
      l.text = labelText;
      l.minWidth = 90;
      l.textAlignment = TextAlign_Right | TextAlign_VertCenter;
      s.add( l );
      s.add( control, 100 );
      return s;
   }

   this.ok = new PushButton( this );
   this.ok.text = "Pull into WBPP";
   this.ok.defaultButton = true;
   this.ok.onClick = function() { self.ok(); };

   this.cancel = new PushButton( this );
   this.cancel.text = "Cancel";
   this.cancel.onClick = function() { self.cancel(); };

   let buttons = new HorizontalSizer;
   buttons.spacing = 6;
   buttons.addStretch();
   buttons.add( this.ok );
   buttons.add( this.cancel );

   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.spacing = 8;
   this.sizer.add( this.info );
   this.sizer.add( row( "Base URL:", this.urlEdit ) );
   this.sizer.add( row( "Query id:", this.idEdit ) );
   this.sizer.add( row( "API token:", this.tokenEdit ) );
   this.sizer.add( buttons );

   this.commit = function() {
      settings.baseUrl = this.urlEdit.text.trim();
      settings.queryId = this.idEdit.value;
      settings.token   = this.tokenEdit.text.trim();
   };
}
SBDialog.prototype = new Dialog;

// ---- Entry point ------------------------------------------------------------

function main() {
   let settings = new SBSettings;
   settings.load();

   let dlg = new SBDialog( settings );
   if ( !dlg.execute() )
      return;
   dlg.commit();
   settings.save();

   console.show();
   console.writeln( "StarBase: fetching query ", settings.queryId, " from ", settings.baseUrl );

   let paths = fetchPaths( settings );
   if ( paths.length == 0 ) {
      (new MessageBox( "The query returned no frames.", SB_TITLE, StdIcon_Warning )).execute();
      return;
   }
   console.writeln( "StarBase: ", paths.length, " frame(s) resolved; handing to WBPP." );
   handToWBPP( paths );
}

main();
