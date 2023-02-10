

with Ada.Command_Line;
with Ada.Characters.Handling;
with Ada.Strings.Fixed;
with Ada.Text_IO.Text_Streams;

procedure DoIt is
   use Ada;

   In_Text   : Boolean := False;
   Set_Upper : Boolean := True;
   Is_XML    : Boolean := False;

   generic
      Pattern : String;
   function Start_Translation_G
     (Start : in Positive;
      Line  : in String) return Natural;

   -------------------------
   -- Start_Translation_G --
   -------------------------

   function Start_Translation_G
     (Start : in Positive;
      Line  : in String) return Natural
   is
      R : Natural := 0;
   begin
      --  Check for start of translation <pattern>

      R := Strings.Fixed.Index (Line, Pattern, From =>  Start);

      if R > 0 then
         In_Text := True;
         Set_Upper := True;
         R := R + Pattern'Length;

      elsif In_Text then
         --  Check for quote

         R := Strings.Fixed.Index (Line, """", From => Start);

         --  Ok only if all characters before the " are spaces

         if R > 0
           --  and then (for all C of Line (Line'First .. R - 1) => C = ' ')
         then
            In_Text := True;
            R := R + 1;
         end if;
      end if;

      return R;
   end Start_Translation_G;

   -----------------------
   -- Start_Translation --
   -----------------------

   function Start_Translation is
     new Start_Translation_G ("_(""");

   function Start_Translation_M is
     new Start_Translation_G ("ngettext(""");

   function Start_Translation_IOP is
     new Start_Translation_G ("$DESCRIPTION: """);

   function Start_Translation_IOP2 is
     new Start_Translation_G ("$DESCRIPTION:""");

   function Start_Translation_XML is
     new Start_Translation_G ("description>");

   function Start_Translation_XML_O is
     new Start_Translation_G ("<option>");

   function Start_Translation_XML_D is
     new Start_Translation_G ("<default>");

   --------------------------
   -- Start_Translation_PO --
   --------------------------

   function Start_Translation_PO is
     new Start_Translation_G ("msgid """);
   function Start_Translation_PO_P is
     new Start_Translation_G ("msgid_plural """);

   ---------------------
   -- End_Translation --
   ---------------------

   function End_Translation (Line : in String) return Natural is
      R : Natural := 0;
   begin
      --  Check for end of translation "),

      R := Strings.Fixed.Index (Line, """),", Going => Strings.Backward);

      if R > 0 then
         R := R - 1;

      else
         --  Check for quote

         R := Strings.Fixed.Index (Line, """", Going => Strings.Backward);

         --  Ok only if all characters after the " are spaces

         if R > 0
           and then (for all C of Line (R + 1 .. Line'Last) => C = ' ')
         then
            R := R - 1;
         end if;
      end if;

      return R;
   end End_Translation;

   function End_Translation
     (Start : in Natural;
      Line  : in String) return Natural
   is
      Esc : Boolean := False;
   begin
      if Start > 0 then
         for K in Start .. Line'Last loop
            declare
               C : constant Character := Line (K);
            begin
               if C = '\' then
                  Esc := True;

               elsif not Esc and then C = '"' then
                  return K - 1;

               else
                  Esc := False;
               end if;
            end;
         end loop;
      end if;

      return 0;
   end End_Translation;

   -------------------------
   -- End_Translation_XML --
   -------------------------

   function End_Translation_XML
     (Start : in Natural;
      Line  : in String) return Natural
   is
      R : Natural := 0;
   begin
      R := Strings.Fixed.Index (Line, "</", Going => Strings.Backward);

      if R > 0 then
         R := R - 1;
      end if;

      return R;
   end End_Translation_XML;

   ------------------------
   -- End_Translation_PO --
   ------------------------

   function End_Translation_PO (Line : in String) return Natural is
      R : Natural := 0;
   begin
      --  Check for end of translation ",

      R := Strings.Fixed.Index (Line, """", Going => Strings.Backward);

      if R > 0 then
         R := R - 1;
      end if;

      return R;
   end End_Translation_PO;

   --------------
   -- Set_Case --
   --------------

   procedure Set_Case (Txt : in out String) is
      Format : Boolean := False;
   begin
      --  Check for exception
      if Txt in "libsecret" | "kwallet" | "mm" | "cm"
        | "graphics;photography;raw;"
        | "library.db"
      then
         return;
      end if;

      --  The text to translated

      for K in Txt'Range loop
         declare
            C : Character := Txt (K);
         begin
            if C = '%' then
               Format := True;

            elsif C = '<' then
               Set_Upper := False;

            elsif Is_XML and then C = '&' then
               Format := True;

            elsif not Format
              and then Set_Upper
              and then C in 'a' .. 'z' | '0' .. '9'
              and then (K = Txt'First
                        or else Txt (K - 1) /= '\')
            then
               C := Characters.Handling.To_Upper (C);
               Set_Upper := False;

            elsif C in '.' | ']' then
               Set_Upper := True;

            elsif C in 'A' .. 'Z' | '+' then
               Set_Upper := False;

            elsif C = ' ' then
               Format := False;
            end if;

            Txt (K) := C;
         end;
      end loop;
   end Set_Case;

   -----------------
   -- Handle_Line --
   -----------------

   procedure Handle_Line (Line : in out String) is

      ---------------
      -- Get_First --
      ---------------

      function Get_First
        (Start : in Positive;
         Line  : in String) return Natural
      is
         F : Natural := Start_Translation_IOP (Start, Line);
      begin
         if F = 0 then
            F := Start_Translation_IOP2 (Start, Line);

            if F = 0 then
               F := Start_Translation (Start, Line);

               if F = 0 then
                  F := Start_Translation_M (Start, Line);
               end if;
            end if;
         end if;

         return F;
      end Get_First;

      -------------
      -- Get_End --
      -------------

      function Get_End (Start : Natural) return Character is
         R : Natural := Start;
      begin
         while R > 0 and then R < Line'Last and then Line (R) = ' ' loop
            R := R + 1;
         end loop;

         if R in Line'Range then
            return Line (R);
         else
            return '.';
         end if;
      end Get_End;

      First : Natural := Get_First (Line'First, Line);
      Last  : Natural := End_Translation (First + 1, Line);
      After : Natural;

   begin
      if Line'Length > 0
        and then First > 0
        and then In_Text
      then
         --  We are in a translation... Let's camel-case

         --   Text_IO.Put_Line ("H: " & In_Text'Img & Line);
         --   Text_IO.Put_Line ("   " & Line (First .. Last));

         Replace : loop
            if First > Line'First + 5
              and then
                Line (First - 4 .. First - 1) = "C_("""
            then
               null;
            else
               Set_Case (Line (First .. Last));
            end if;

            --  In_Text := (Last > 0
            --              and then Last + 1 = Line'Last)
              --  or else
              --    (Last < Line'Last + 1
              --     and then Line (Last + 2) /= ')');

            After := Last + 2;

            In_Text := Get_End (After) /= ')';
            --  Set_Upper := In_Text;

            if After < Line'Last then
               First := Get_First (After, Line);

               if First = 0 then
                  In_Text := False;
                  Set_Upper := False;
                  exit Replace;
               end if;

               Last := End_Translation (First + 1, Line);

               if Last = 0 then
                  In_Text := False;
                  Set_Upper := False;
                  exit Replace;
               end if;
            else
               exit Replace;
            end if;
         end loop Replace;

      else
         --  Check if opening a translation at end of line _(

         if Line'Length > 2
           and then Line (Line'Last - 1 .. Line'Last) = "_("
         then
            In_Text := True;
            Set_Upper := True;
         else
            --  Standard line, just display it

            Set_Upper := False;
            In_Text := False;
         end if;
      end if;
   end Handle_Line;

   ---------------------
   -- Handle_Line_XML --
   ---------------------

   procedure Handle_Line_XML (Line : in out String) is

      ---------------
      -- Get_First --
      ---------------

      function Get_First
        (Start : in Positive;
         Line  : in String) return Natural
      is
         F : Natural := Start_Translation_XML (Start, Line);
      begin
         if F = 0 then
            F := Start_Translation_XML_O (Start, Line);

            if F = 0 then
               F := Start_Translation_XML_D (Start, Line);
            end if;
         end if;

         return F;
      end Get_First;

      First : constant Natural := Get_First (Line'First, Line);
      Last  : constant Natural := End_Translation_XML (First, Line);
   begin
      if Line'Length > 0
        and then First > 0
        and then Last > First
      then
         Set_Case (Line (First .. Last));
      end if;
   end Handle_Line_XML;

   --------------------
   -- Handle_Line_PO --
   --------------------

   procedure Handle_Line_PO (Line : in out String) is

      ---------------
      -- Get_First --
      ---------------

      function Get_First
        (Start : in Positive;
         Line  : in String) return Natural
      is
         F : Natural := Start_Translation_PO (Start, Line);
      begin
         if F = 0 then
            F := Start_Translation_PO_P (Start, Line);
         end if;

         return F;
      end Get_First;

      First  : constant Natural := Get_First (Line'First, Line);
      Last   : constant Natural := End_Translation (First, Line);
      Is_End : constant Boolean :=
                 Strings.Fixed.Index (Line, "msgstr") = Line'First;
   begin
      if Line'Length > 0
        and then First > 0
        and then In_Text
        and then not Is_End
      then
         --  Start of line

         Set_Case (Line (First .. Last));

         --  Check if we have an end of translation

         In_Text := not Is_End;

      else
         --  Standard line, just display it

         Set_Upper := False;
         In_Text := False;
      end if;
   end Handle_Line_PO;

   Kind     : constant String := Command_Line.Argument (1);
   Filename : constant String := Command_Line.Argument (2);
   File     : Text_IO.File_Type;

begin
   Is_XML := Kind = "X";

   Text_IO.Open (File, Text_IO.In_File, Filename);

   while not Text_IO.End_Of_File (File) loop
      declare
         Line : String := Text_IO.Get_Line (File);
      begin
         if Kind = "C" then
            Handle_Line (Line);
         elsif Kind = "X" then
            Handle_Line_XML (Line);
         else
            Handle_Line_PO (Line);
         end if;

         Text_IO.Put_Line (Line);
      end;
   end loop;

   Text_IO.Close (File);
end DoIt;
